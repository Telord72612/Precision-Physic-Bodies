#include "PpbFrameSubscriptions.h"

#include <cassert>
#include <stdexcept>

struct Context {
    PpbFrameSubscriptions* subscriptions = nullptr;
    unsigned long long token = 0;
    int calls = 0;
    bool removeSelf = false;
    bool throws = false;
};

void Receive(const PPBAPI::PpbFrameEvent* event, const PPBAPI::PpbTouchContact*, void* user)
{
    auto& context = *static_cast<Context*>(user);
    assert(event->kind == PPBAPI::kFramePublished);
    ++context.calls;
    if (context.removeSelf)
        assert(context.subscriptions->Remove(context.token) == PPBAPI::PpbRemovalState::Pending);
    if (context.throws) throw std::runtime_error("client failure");
}

int main()
{
    PpbFrameSubscriptions subscriptions;
    PPBAPI::PpbFrameEvent event{};
    event.kind = PPBAPI::kFramePublished;
    Context contexts[PpbFrameSubscriptions::kCapacity]{};
    assert(!subscriptions.Add(nullptr, nullptr));
    assert(subscriptions.Remove(0) == PPBAPI::PpbRemovalState::Unknown);
    assert(subscriptions.Remove(1) == PPBAPI::PpbRemovalState::Unknown);
    for (auto& context : contexts) {
        context.subscriptions = &subscriptions;
        context.token = subscriptions.Add(Receive, &context);
        assert(context.token);
    }
    assert(!subscriptions.Add(Receive, nullptr));
    const auto oldRecipients = subscriptions.Recipients();
    for (auto token : oldRecipients) subscriptions.Deliver(token, event, nullptr);
    for (auto& context : contexts) assert(context.calls == 1);

    contexts[0].removeSelf = true;
    subscriptions.Deliver(contexts[0].token, event, nullptr);
    assert(contexts[0].calls == 2);
    assert(subscriptions.Remove(contexts[0].token) == PPBAPI::PpbRemovalState::Drained);
    const auto retired = contexts[0].token;
    Context replacement{&subscriptions};
    replacement.token = subscriptions.Add(Receive, &replacement);
    assert(replacement.token && replacement.token != retired);
    assert(subscriptions.Remove(retired) == PPBAPI::PpbRemovalState::Drained);
    subscriptions.Deliver(retired, event, nullptr);
    assert(replacement.calls == 0);
    subscriptions.Deliver(replacement.token, event, nullptr);
    assert(replacement.calls == 1);

    // A registration created after queue capture cannot receive that old frame.
    for (auto token : oldRecipients) subscriptions.Deliver(token, event, nullptr);
    assert(replacement.calls == 1);
    assert(subscriptions.Remove(contexts[1].token) == PPBAPI::PpbRemovalState::Drained);
    subscriptions.Deliver(contexts[1].token, event, nullptr);
    assert(contexts[1].calls == 2);
    contexts[2].throws = true;
    subscriptions.Deliver(contexts[2].token, event, nullptr);
    assert(subscriptions.Remove(contexts[2].token) == PPBAPI::PpbRemovalState::Drained);
    const auto priorCalls = contexts[2].calls;
    subscriptions.Deliver(contexts[2].token, event, nullptr);
    assert(contexts[2].calls == priorCalls);
}
