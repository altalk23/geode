#include <Geode/loader/Dispatch.hpp>
#include <Geode/utils/ranges.hpp>
#include <mutex>

using namespace geode::prelude;

bool DispatchListenerPool::add(EventListenerProtocol* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_locked) {
        m_toAdd.push_back(listener);
        ranges::remove(m_toRemove, listener);
    }
    else {
        if (auto val = typeinfo_cast<DispatchListenerProtocol*>(listener)) {
            auto& list = m_listeners[val->getID()];
            list.insert(list.begin(), listener);
        }
    }
    return true;
}

void DispatchListenerPool::remove(EventListenerProtocol* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_locked) {
        m_toRemove.push_back(listener);
        ranges::remove(m_toAdd, listener);
    }
    else {
        if (auto val = typeinfo_cast<DispatchListenerProtocol*>(listener)) {
            auto& list = m_listeners[val->getID()];
            ranges::remove(list, listener);
        }
    }
}

void DispatchListenerPool::cleanup() {
    for (auto listener : m_toAdd) {
        if (auto val = typeinfo_cast<DispatchListenerProtocol*>(listener)) {
            auto& list = m_listeners[val->getID()];
            list.insert(list.begin(), listener);
        }
    }
    m_toAdd.clear();

    for (auto listener : m_toRemove) {
        if (auto val = typeinfo_cast<DispatchListenerProtocol*>(listener)) {
            auto& list = m_listeners[val->getID()];
            ranges::remove(list, listener);
        }
    }
    m_toRemove.clear();
}

ListenerResult DispatchListenerPool::handle(Event* event) {
    std::unique_lock<std::mutex> lock(m_mutex);
    auto res = ListenerResult::Propagate;
    m_locked += 1;
    if (auto val = typeinfo_cast<BaseDispatchEvent*>(event)) {
        auto& list = m_listeners[val->getID()];
        for (auto h : list) {
            lock.unlock();
            // if an event listener gets destroyed in the middle of this loop, it 
            // gets set to null
            if (h && h->handle(event) == ListenerResult::Stop) {
                res = ListenerResult::Stop;
                lock.lock();
                break;
            }
            lock.lock();
        }
    }
    m_locked -= 1;
    // only mutate listeners once nothing is iterating 
    // (if there are recursive handle calls)
    if (m_locked > 0) return res;

    this->cleanup();
    
    return res;
}

DispatchListenerPool* DispatchListenerPool::get() {
    static auto inst = new DispatchListenerPool();
    return inst;
}

EventListenerPool* DispatchListenerProtocol::getPool() const {
    return DispatchListenerPool::get();
}