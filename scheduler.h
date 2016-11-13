#ifndef ALEXAAGENT__SCHEDULER_H
#define ALEXAAGENT__SCHEDULER_H

#include <memory>
#include <set>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

class t_task
{
	boost::asio::yield_context& v_yield;
	boost::asio::steady_timer v_timer;
	std::deque<std::function<void(boost::asio::yield_context&)>> v_actions;

public:
	t_task(boost::asio::yield_context& a_yield, boost::asio::io_service& a_io) : v_yield(a_yield), v_timer(a_io)
	{
	}
	void f_wait(const boost::asio::steady_timer::duration& a_duration = std::chrono::duration<int>::max())
	{
		v_timer.expires_from_now(a_duration);
		boost::system::error_code ec;
		v_timer.async_wait(v_yield[ec]);
		while (!v_actions.empty()) {
			auto action = std::move(v_actions.front());
			v_actions.pop_front();
			action(v_yield);
		}
	}
	void f_notify()
	{
		v_timer.cancel();
	}
	void f_post(std::function<void(boost::asio::yield_context&)>&& a_action)
	{
		v_actions.push_back(std::move(a_action));
		f_notify();
	}
};

class t_scheduler : public boost::asio::io_service::strand
{
	std::set<std::unique_ptr<boost::asio::steady_timer>> v_timers;
	std::set<t_task*> v_tasks;

	template<typename T_callback>
	void f_run_every(std::set<std::unique_ptr<boost::asio::steady_timer>>::iterator a_i, const boost::asio::steady_timer::duration& a_duration, T_callback a_callback)
	{
		(*a_i)->expires_from_now(a_duration);
		(*a_i)->async_wait(wrap([this, a_i, a_duration, a_callback](auto a_ec)
		{
			if (a_callback(a_ec))
				this->f_run_every(a_i, a_duration, a_callback);
			else
				v_timers.erase(a_i);
		}));
	}

public:
	class t_stop
	{
	};

	t_scheduler(boost::asio::io_service& a_io) : boost::asio::io_service::strand(a_io)
	{
	}
	boost::asio::io_service& f_io()
	{
		return get_io_service();
	}
	template<typename T_callback>
	boost::asio::steady_timer& f_run_in(const boost::asio::steady_timer::duration& a_duration, T_callback a_callback)
	{
		auto i = v_timers.insert(std::make_unique<boost::asio::steady_timer>(f_io(), a_duration)).first;
		(*i)->async_wait(wrap([this, i, a_callback](auto a_ec)
		{
			a_callback(a_ec);
			v_timers.erase(i);
		}));
		return **i;
	}
	template<typename T_callback>
	boost::asio::steady_timer& f_run_every(const boost::asio::steady_timer::duration& a_duration, T_callback a_callback)
	{
		auto i = v_timers.insert(std::make_unique<boost::asio::steady_timer>(f_io())).first;
		f_run_every(i, a_duration, a_callback);
		return **i;
	}
	template<typename T_main>
	void f_spawn(T_main&& a_main)
	{
		boost::asio::spawn(static_cast<boost::asio::io_service::strand&>(*this), [this, a_main = std::move(a_main)](auto a_yield)
		{
			t_task task(a_yield, this->f_io());
			auto i = v_tasks.insert(&task).first;
			try {
				a_main(task);
			} catch (...) {
			}
			v_tasks.erase(i);
		});
	}
	template<typename T_completed>
	void f_shutdown(T_completed&& a_completed)
	{
		if (v_tasks.empty()) {
			a_completed();
			return;
		}
		auto& stopping = f_run_every(std::chrono::duration<int>::max(), [this, a_completed = std::move(a_completed)](auto)
		{
			if (!v_tasks.empty()) return true;
			a_completed();
			return false;
		});
		for (auto& x : v_tasks) x->f_post([&](auto)
		{
			stopping.cancel();
			throw t_stop();
		});
	}
};

#endif
