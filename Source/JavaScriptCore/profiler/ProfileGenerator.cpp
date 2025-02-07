/*
 * Copyright (C) 2008, 2014, 2016 Apple Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ProfileGenerator.h"

#include "CallFrame.h"
#include "CodeBlock.h"
#include "JSGlobalObject.h"
#include "JSStringRef.h"
#include "JSFunction.h"
#include "LegacyProfiler.h"
#include "JSCInlines.h"
#include "Profile.h"
#include "StackVisitor.h"

namespace JSC {

Ref<ProfileGenerator> ProfileGenerator::create(ExecState* exec, const String& title, unsigned uid, PassRefPtr<Stopwatch> stopwatch)
{
    return adoptRef(*new ProfileGenerator(exec, title, uid, stopwatch));
}

ProfileGenerator::ProfileGenerator(ExecState* exec, const String& title, unsigned uid, PassRefPtr<Stopwatch> stopwatch)
    : m_origin(exec ? exec->lexicalGlobalObject() : nullptr)
    , m_profileGroup(exec ? exec->lexicalGlobalObject()->profileGroup() : 0)
    , m_stopwatch(stopwatch)
    , m_foundConsoleStartParent(false)
    , m_suspended(false)
{
    double startTime = m_stopwatch->elapsedTime();
    m_profile = Profile::create(title, uid, startTime);
    m_currentNode = m_rootNode = m_profile->rootNode();
    if (exec)
        addParentForConsoleStart(exec, startTime);
}

class AddParentForConsoleStartFunctor {
public:
    AddParentForConsoleStartFunctor(ExecState* exec, RefPtr<ProfileNode>& rootNode, RefPtr<ProfileNode>& currentNode, double startTime)
        : m_exec(exec)
        , m_hasSkippedFirstFrame(false)
        , m_foundParent(false)
        , m_rootNode(rootNode)
        , m_currentNode(currentNode)
        , m_startTime(startTime)
    {
    }

    bool foundParent() const { return m_foundParent; }

    StackVisitor::Status operator()(StackVisitor& visitor) const
    {
        if (!m_hasSkippedFirstFrame) {
            m_hasSkippedFirstFrame = true;
            return StackVisitor::Continue;
        }

        unsigned line = 0;
        unsigned column = 0;
        visitor->computeLineAndColumn(line, column);
        m_currentNode = ProfileNode::create(m_exec, LegacyProfiler::createCallIdentifier(m_exec, visitor->callee(), visitor->sourceURL(), line, column), m_rootNode.get());
        m_currentNode->appendCall(ProfileNode::Call(m_startTime));
        m_rootNode->spliceNode(m_currentNode.get());

        m_foundParent = true;
        return StackVisitor::Done;
    }

private:
    ExecState* m_exec;
    mutable bool m_hasSkippedFirstFrame;
    mutable bool m_foundParent;
    RefPtr<ProfileNode>& m_rootNode;
    RefPtr<ProfileNode>& m_currentNode;
    double m_startTime;
};

void ProfileGenerator::addParentForConsoleStart(ExecState* exec, double startTime)
{
    AddParentForConsoleStartFunctor functor(exec, m_rootNode, m_currentNode, startTime);
    exec->iterate(functor);

    m_foundConsoleStartParent = functor.foundParent();
}

const String& ProfileGenerator::title() const
{
    return m_profile->title();
}

void ProfileGenerator::beginCallEntry(ProfileNode* node, double startTime)
{
    ASSERT_ARG(node, node);

    if (std::isnan(startTime))
        startTime = m_stopwatch->elapsedTime();

    node->appendCall(ProfileNode::Call(startTime));
}

void ProfileGenerator::endCallEntry(ProfileNode* node)
{
    ASSERT_ARG(node, node);

    ProfileNode::Call& last = node->lastCall();

    double previousElapsedTime = std::isnan(last.elapsedTime()) ? 0.0 : last.elapsedTime();
    double newlyElapsedTime = m_stopwatch->elapsedTime() - last.startTime();
    last.setElapsedTime(previousElapsedTime + newlyElapsedTime);
}

void ProfileGenerator::willExecute(ExecState* callerCallFrame, const CallIdentifier& callIdentifier)
{
    if (!m_origin)
        return;

    if (m_suspended)
        return;

    RefPtr<ProfileNode> calleeNode = nullptr;

    // Find or create a node for the callee call frame.
    for (const RefPtr<ProfileNode>& child : m_currentNode->children()) {
        if (child->callIdentifier() == callIdentifier)
            calleeNode = child;
    }

    if (!calleeNode) {
        calleeNode = ProfileNode::create(callerCallFrame, callIdentifier, m_currentNode.get());
        m_currentNode->addChild(calleeNode);
    }

    m_currentNode = calleeNode;
    beginCallEntry(calleeNode.get(), m_stopwatch->elapsedTime());
}

void ProfileGenerator::didExecute(ExecState* callerCallFrame, const CallIdentifier& callIdentifier)
{
    if (!m_origin)
        return;

    if (m_suspended)
        return;

    // Make a new node if the caller node has never seen this callee call frame before.
    // This can happen if |console.profile()| is called several frames deep in the call stack.
    ASSERT(m_currentNode);
    if (m_currentNode->callIdentifier() != callIdentifier) {
        RefPtr<ProfileNode> calleeNode = ProfileNode::create(callerCallFrame, callIdentifier, m_currentNode.get());
        beginCallEntry(calleeNode.get(), m_currentNode->lastCall().startTime());
        endCallEntry(calleeNode.get());
        m_currentNode->spliceNode(calleeNode.release());
        return;
    }

    endCallEntry(m_currentNode.get());
    m_currentNode = m_currentNode->parent();
}

void ProfileGenerator::exceptionUnwind(ExecState* handlerCallFrame, const CallIdentifier&)
{
    if (m_suspended)
        return;

    // If the current node was called by the handler (==) or any
    // more nested function (>) the we have exited early from it.
    ASSERT(m_currentNode);
    while (m_currentNode->callerCallFrame() >= handlerCallFrame) {
        didExecute(m_currentNode->callerCallFrame(), m_currentNode->callIdentifier());
        ASSERT(m_currentNode);
    }
}

void ProfileGenerator::stopProfiling()
{
    for (ProfileNode* node = m_currentNode.get(); node != m_profile->rootNode(); node = node->parent())
        endCallEntry(node);

    if (m_foundConsoleStartParent) {
        removeProfileStart();
        removeProfileEnd();
    }

    ASSERT(m_currentNode);

    // Set the current node to the parent, because we are in a call that
    // will not get didExecute call.
    m_currentNode = m_currentNode->parent();
}

// The console.profile that started this ProfileGenerator will be the first child.
void ProfileGenerator::removeProfileStart()
{
    ProfileNode* currentNode = nullptr;
    for (ProfileNode* next = m_rootNode.get(); next; next = next->firstChild())
        currentNode = next;

    if (currentNode->callIdentifier().functionName() != "profile")
        return;

    currentNode->parent()->removeChild(currentNode);
}

// The console.profileEnd that stopped this ProfileGenerator will be the last child.
void ProfileGenerator::removeProfileEnd()
{
    ProfileNode* currentNode = nullptr;
    for (ProfileNode* next = m_rootNode.get(); next; next = next->lastChild())
        currentNode = next;

    if (currentNode->callIdentifier().functionName() != "profileEnd")
        return;

    ASSERT(currentNode->callIdentifier() == (currentNode->parent()->children()[currentNode->parent()->children().size() - 1])->callIdentifier());
    currentNode->parent()->removeChild(currentNode);
}

} // namespace JSC
