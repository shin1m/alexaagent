#ifndef ALEXAAGENT__CHANNEL_H
#define ALEXAAGENT__CHANNEL_H

#include <deque>

#include "scheduler.h"
#include "decoder.h"

template<typename T_audio>
class t_channel
{
	t_task& v_task;
	std::deque<std::unique_ptr<T_audio>> v_audios;
	std::unique_ptr<T_audio> v_playing;
	long v_offset = -1;

public:
	t_channel(t_task& a_task) : v_task(a_task)
	{
	}
	t_task& f_task() const
	{
		return v_task;
	}
	bool f_empty() const
	{
		return v_audios.empty() && !v_playing;
	}
	T_audio* f_playing() const
	{
		return v_playing.get();
	}
	long f_offset() const
	{
		return v_offset;
	}
	void f_queue(std::unique_ptr<T_audio>&& a_audio)
	{
		v_audios.push_back(std::move(a_audio));
		v_task.f_notify();
	}
	void f_clear()
	{
		v_audios.clear();
	}
	template<typename T_started, typename T_finishing, typename T_finished, typename T_stopped, typename T_failed, typename T_finally, typename T_stuttering, typename T_stuttered>
	void f_run(T_started a_started, T_finishing a_finishing, T_finished a_finished, T_stopped a_stopped, T_failed a_failed, T_finally a_finally, T_stuttering a_stuttering, T_stuttered a_stuttered)
	{
		while (true) {
			while (v_audios.empty()) v_task.f_wait();
			v_playing = std::move(v_audios.front());
			v_audios.pop_front();
			v_offset = 0;
			a_started();
			try {
				auto decoder = std::make_unique<t_audio_decoder>(v_playing->f_open(v_task, a_stuttering, a_stuttered));
				t_audio_target target;
				(*decoder)([&](size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
				{
					target(a_channels, a_bytes, a_p, a_n, a_rate);
					v_offset = target.f_offset() * 1000.0;
					if (target.f_remain() > 2.0) v_task.f_wait(std::chrono::seconds(1));
				});
				a_finishing();
				while (target.f_flush() > 0) {
					v_offset = target.f_offset() * 1000.0;
					v_task.f_wait(std::chrono::milliseconds(static_cast<int>(target.f_remain() * 250.0)));
				}
				a_finished();
			} catch (std::nullptr_t) {
				a_stopped();
			} catch (std::exception& e) {
				a_failed(e);
			}
			v_playing.reset();
			a_finally();
		}
	}
};

#endif
