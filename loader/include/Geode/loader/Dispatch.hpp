#pragma once

#include "Event.hpp"

#include <functional>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <string>
#include <tuple>

namespace geode {
    // Mod interoperability

    class GEODE_DLL DispatchListenerPool : public EventListenerPool {
    protected:
        std::atomic_size_t m_locked = 0;
        std::unordered_map<std::string, std::vector<EventListenerProtocol*>> m_listeners;
        std::vector<EventListenerProtocol*> m_toAdd;
        std::vector<EventListenerProtocol*> m_toRemove;
        std::mutex m_mutex;

        void cleanup();

    public:
        bool add(EventListenerProtocol* listener) override;
        void remove(EventListenerProtocol* listener) override;
        ListenerResult handle(Event* event) override;

        static DispatchListenerPool* get();
    };

    class GEODE_DLL BaseDispatchEvent : public Event {
    public:
        virtual std::string getID() const = 0;
    };

    template <class... Args>
    class DispatchEvent : public BaseDispatchEvent {
    protected:
        std::string m_id;
        std::tuple<Args...> m_args;
    
    public:
        DispatchEvent(std::string const& id, Args... args)
          : m_id(id), m_args(std::make_tuple(args...)) {}
        
        std::tuple<Args...> getArgs() const {
            return m_args;
        }

        std::string getID() const {
            return m_id;
        }
    };

    template <class... Args>
    class DispatchFilter : public EventFilter<DispatchEvent<Args...>> {
    protected:
        std::string m_id;

    public:
        using Ev = DispatchEvent<Args...>;
        using Callback = ListenerResult(Args...);

        EventListenerPool* getPool() const {
            return DispatchListenerPool::get();
        }

        ListenerResult handle(utils::MiniFunction<Callback> fn, Ev* event) {
            if (event->getID() == m_id) {
                return std::apply(fn, event->getArgs());
            }
            return ListenerResult::Propagate;
        }

        DispatchFilter(std::string const& id) : m_id(id) {}
        DispatchFilter(DispatchFilter const&) = default;

        std::string getID() const {
            return m_id;
        }
    };

    class GEODE_DLL DispatchListenerProtocol : public EventListenerProtocol {
    public:
        EventListenerPool* getPool() const override;
        virtual std::string getID() const = 0;
    };

    template <typename T>
    concept IsDispatch = std::is_base_of_v<DispatchFilter<typename T::Event>, T> &&
        requires(T a) {
            a.handle(std::declval<typename T::Callback>(), std::declval<typename T::Event*>());
        };

    template <IsDispatch T>
    class DispatchListener : public DispatchListenerProtocol {
    public:
        using Callback = typename T::Callback;
        template <typename C>
            requires std::is_class_v<C>
        using MemberFn = typename to_member<C, Callback>::value;

        ListenerResult handle(Event* e) override {
            if (m_callback) {
                if (auto myev = cast::typeinfo_cast<typename T::Event*>(e)) {
                    return m_filter.handle(m_callback, myev);
                }
            }
            return ListenerResult::Propagate;
        }

        EventListenerPool* getPool() const override {
            return m_filter.getPool();
        }

        DispatchListener(T filter = T()) : m_filter(filter) {
            m_filter.setListener(this);
            this->enable();
        }

        DispatchListener(utils::MiniFunction<Callback> fn, T filter = T())
          : m_callback(fn), m_filter(filter)
        {
            m_filter.setListener(this);
            this->enable();
        }

        DispatchListener(Callback* fnptr, T filter = T()) : m_callback(fnptr), m_filter(filter) {
            m_filter.setListener(this);
            this->enable();
        }

        template <class C>
        DispatchListener(C* cls, MemberFn<C> fn, T filter = T()) :
            DispatchListener(std::bind(fn, cls, std::placeholders::_1), filter)
        {
            m_filter.setListener(this);
            this->enable();
        }

        DispatchListener(DispatchListener&& other)
          : m_callback(std::move(other.m_callback)),
            m_filter(std::move(other.m_filter))
        {
            m_filter.setListener(this);
            other.disable();
            this->enable();
        }

        DispatchListener(DispatchListener const& other)
          : m_callback(other.m_callback),
            m_filter(other.m_filter)
        {
            m_filter.setListener(this);
            this->enable();
        }

        void bind(utils::MiniFunction<Callback> fn) {
            m_callback = fn;
        }

        template <typename C>
        void bind(C* cls, MemberFn<C> fn) {
            m_callback = std::bind(fn, cls, std::placeholders::_1);
        }

        void setFilter(T filter) {
            m_filter = filter;
            m_filter.setListener(this);
        }

        T& getFilter() {
            return m_filter;
        }

        T const& getFilter() const {
            return m_filter;
        }

        utils::MiniFunction<Callback>& getCallback() {
            return m_callback;
        }

        std::string getID() const override {
            return m_filter.getID();
        }

    protected:
        utils::MiniFunction<Callback> m_callback = nullptr;
        T m_filter;
    };
}