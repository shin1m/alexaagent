#ifndef ALEXAAGENT__SESSION_H
#define ALEXAAGENT__SESSION_H

#include <deque>
#include <ostream>
#include <boost/asio/system_timer.hpp>
#include <nghttp2/asio_http2_client.h>

#include "json.h"
#include "multipart.h"
#include "audio.h"
#include "scheduler.h"

enum t_severity
{
	e_severity__TRACE,
	e_severity__INFORMATION,
	e_severity__ERROR,
	e_severity__NEVER
};

class t_session
{
	struct t_channel
	{
		t_task& v_task;
		t_audio_target v_target;
		std::deque<std::function<void()>> v_directives;
		std::string v_playing;

		t_channel(t_task& a_task) : v_task(a_task)
		{
		}
		long f_offset() const
		{
			return v_target.f_offset() * 1000.0;
		}
		void f_queue(std::function<void()>&& a_directive)
		{
			v_directives.push_back(std::move(a_directive));
			v_task.f_notify();
		}
		void f_clear()
		{
			v_directives.clear();
		}
		void f_run()
		{
			while (true) {
				while (v_directives.empty()) v_task.f_wait();
				auto directive = std::move(v_directives.front());
				v_directives.pop_front();
				directive();
			}
		}
		void f_loop(t_audio_decoder& a_decoder)
		{
			a_decoder([this](size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
			{
				auto queued = v_target(a_channels, a_bytes, a_p, a_n, a_rate);
				while (queued > 32) {
					v_task.f_wait(std::chrono::milliseconds(static_cast<int>(v_target.f_remain() * 500.0)));
					queued = v_target.f_flush();
				}
			});
		}
		void f_flush()
		{
			while (v_target.f_flush() > 0) v_task.f_wait(std::chrono::milliseconds(static_cast<int>(v_target.f_remain() * 500.0)));
		}
		void f_stop()
		{
			if (v_playing.empty()) return;
			v_target.f_stop();
			v_task.f_post([](auto)
			{
				throw nullptr;
			});
		}
	};
public:
	class t_alert
	{
		friend class t_session;

		std::string v_type;
		std::string v_at;
		std::unique_ptr<boost::asio::system_timer> v_timer;
		std::unique_ptr<ALuint, void (*)(ALuint*)> v_source{nullptr, [](auto a_x)
		{
			alDeleteSources(1, a_x);
		}};

		t_alert(const std::string& a_type, const std::string& a_at) : v_type(a_type), v_at(a_at)
		{
		}
		void f_play(ALuint a_buffer, ALfloat a_gain)
		{
			if (v_source) {
				alSourceStop(*v_source);
			} else {
				ALuint source;
				alGenSources(1, &source);
				v_source.reset(new ALuint(source));
				alSourcei(source, AL_LOOPING, AL_TRUE);
			}
			alSourcei(*v_source, AL_BUFFER, a_buffer);
			alSourcef(*v_source, AL_GAIN, a_gain);
			alSourcePlay(*v_source);
		}

	public:
		const std::string& f_type() const
		{
			return v_type;
		}
		const std::string& f_at() const
		{
			return v_at;
		}
		bool f_active() const
		{
			return static_cast<bool>(v_source);
		}
	};
private:
	struct t_parser;
	struct t_attached_audio
	{
		t_session& v_session;
		t_task& v_task;
		std::map<std::string, t_attached_audio*>::iterator v_i;
		boost::asio::streambuf v_data;
		bool v_finished = false;
		t_parser* v_parser = nullptr;

		t_attached_audio(t_session& a_session, t_task& a_task, const std::string& a_id) : v_session(a_session), v_task(a_task), v_i(v_session.v_id2audio.emplace(a_id, this).first)
		{
		}
		~t_attached_audio()
		{
			v_session.v_id2audio.erase(v_i);
			if (!v_parser) return;
			v_parser->v_content = &t_parser::f_ignore_content;
			v_parser->v_finish = &t_parser::f_ignore_finish;
		}
		t_audio_source* f_open(std::function<void()>&& a_stuttering, std::function<void()>&& a_stuttered)
		{
			return new t_callback_audio_source([this, a_stuttering = std::move(a_stuttering), a_stuttered = std::move(a_stuttered)](auto a_p, auto a_n)
			{
				if (v_data.size() <= 0) {
					if (v_finished) return 0;
					v_task.f_wait();
					if (v_data.size() <= 0) {
						if (v_finished) return 0;
						a_stuttering();
						do v_task.f_wait(); while (v_data.size() <= 0 && !v_finished);
						a_stuttered();
						if (v_data.size() <= 0) return 0;
					}
				}
				int n = boost::asio::buffer_copy(boost::asio::buffer(a_p, a_n), v_data.data());
				v_data.consume(n);
				return n;
			});
		}
	};
	struct t_parser
	{
		t_session& v_session;
		t_multipart<t_parser> v_multipart;
		void (t_parser::*v_content)(const char*, size_t);
		void (t_parser::*v_finish)() = &t_parser::f_ignore_finish;
		boost::asio::streambuf v_metadata;
		t_attached_audio* v_audio;

		t_parser(t_session& a_session, const std::string& a_boundary) : v_session(a_session), v_multipart(*this, a_boundary)
		{
		}
		void f_json_content(const char* a_p, size_t a_n)
		{
			v_metadata.sputn(a_p, a_n);
		}
		void f_json_finish()
		{
			picojson::value directive;
			picojson::parse(directive, std::istreambuf_iterator<char>(&v_metadata), std::istreambuf_iterator<char>(), nullptr);
			if (v_session.v_log) v_session.v_log(e_severity__TRACE) << "json: " << directive.serialize(true) << std::endl;
			auto& header = directive / "directive" / "header";
			auto ns = header / "namespace"_jss;
			auto name = header / "name"_jss;
			if (v_session.v_log) v_session.v_log(e_severity__INFORMATION) << "parser(" << this << ") directive: " << ns + "." << name << std::endl;
			try {
				v_session.v_handlers.at({ns, name})(directive);
			} catch (std::exception& e) {
				v_session.f_exception_encountered(ns + '.' + name, "UNSUPPORTED_OPERATION", e.what());
			}
		}
		void f_audio_content(const char* a_p, size_t a_n)
		{
			boost::asio::buffer_copy(v_audio->v_data.prepare(a_n), boost::asio::buffer(a_p, a_n));
			v_audio->v_data.commit(a_n);
			v_audio->v_task.f_notify();
		}
		void f_audio_finish()
		{
			v_audio->v_parser = nullptr;
			v_audio->v_finished = true;
			v_audio->v_task.f_notify();
		}
		void f_ignore_content(const char* a_p, size_t a_n)
		{
		}
		void f_ignore_finish()
		{
		}
		void f_boundary()
		{
			(this->*v_finish)();
		}
		void f_part(const std::string& a_type, const std::string& a_id)
		{
			if (v_session.v_log) v_session.v_log(e_severity__TRACE) << "parser(" << this << ") part: " << a_type << ", " << a_id << std::endl;
			if (a_type == "application/json") {
				v_content = &t_parser::f_json_content;
				v_finish = &t_parser::f_json_finish;
				return;
			} else if (a_type == "application/octet-stream") {
				auto i = v_session.v_id2audio.find(a_id);
				if (i != v_session.v_id2audio.end()) {
					v_audio = i->second;
					v_content = &t_parser::f_audio_content;
					v_finish = &t_parser::f_audio_finish;
					v_audio->v_parser = this;
					return;
				}
			}
			v_content = &t_parser::f_ignore_content;
			v_finish = &t_parser::f_ignore_finish;
		}
		void f_content(const char* a_p, size_t a_n)
		{
			(this->*v_content)(a_p, a_n);
		}
		void operator()(const uint8_t* a_p, size_t a_n)
		{
			for (; a_n > 0; --a_n) v_multipart(*a_p++);
		}
	};
	friend struct t_parser;

	const std::string v_boundary_metadata =
		"--this-is-a-boundary\r\n"
		"Content-Disposition: form-data; name=\"metadata\"\r\n"
		"Content-Type: application/json; charset=UTF-8\r\n\r\n";
	const std::string v_boundary_audio =
		"\r\n--this-is-a-boundary\r\n"
		"Content-Disposition: form-data; name=\"audio\"\r\n"
		"Content-Type: application/octet-stream\r\n\r\n";
	const std::string v_boundary_terminator = "\r\n--this-is-a-boundary--\r\n";
	const std::regex v_re_content_type{"\\s*multipart/related\\s*;\\s*boundary\\s*=\\s*([\\-0-9A-Za-z]+).*"};

	boost::asio::ssl::context v_tls;
	t_scheduler& v_scheduler;
	nghttp2::asio_http2::header_map v_header;
	const ALuint* v_sounds;
	std::unique_ptr<nghttp2::asio_http2::client::session> v_session;
	bool v_online = false;
	boost::asio::steady_timer* v_reconnecting = nullptr;
	size_t v_reconnecting_interval = 1;
	size_t v_message_id = 0;
	size_t v_dialog_id = 0;
	std::map<std::pair<std::string, std::string>, std::function<void(const picojson::value&)>> v_handlers{
		{{"SpeechRecognizer", "ExpectSpeech"}, [this](auto a_directive)
		{
			auto& payload = a_directive / "directive" / "payload";
			v_dialog->f_queue([this, dialog_id = a_directive / "directive" / "header" * "dialogRequestId" | std::string(), timeout = static_cast<int>((payload / "timeoutInMilliseconds"_jsn))]
			{
				v_expecting_dialog_id = dialog_id;
				v_expecting_speech = [this, timeout]
				{
					this->f_dialog_acquire(*v_recognizer);
					if (v_log) v_log(e_severity__INFORMATION) << "dialog(" << v_expecting_dialog_id << ") expecting speech within " << timeout << " ms." << std::endl;
					v_expecting_timeout = &v_scheduler.f_run_in(std::chrono::milliseconds(timeout), [this](auto a_ec)
					{
						if (!v_expecting_speech) return;
						v_expecting_speech = nullptr;
						v_expecting_timeout = nullptr;
						this->f_empty_event("SpeechRecognizer", "ExpectSpeechTimedOut");
						this->f_dialog_release();
						if (v_state_changed) v_state_changed();
					});
					if (v_state_changed) v_state_changed();
				};
				do v_dialog->v_task.f_wait(); while (v_expecting_speech);
			});
		}},
		{{"Alerts", "SetAlert"}, [this](auto a_directive)
		{
			auto& payload = a_directive / "directive" / "payload";
			auto token = payload / "token"_jss;
			try {
				this->f_alerts_set(token, payload / "type"_jss, payload / "scheduledTime"_jss);
				if (v_alerts_changed) v_alerts_changed();
				this->f_alerts_event("SetAlertSucceeded", token);
			} catch (std::exception& e) {
				this->f_exception_encountered("Alerts.SetAlert", "INTERNAL_ERROR", e.what());
				this->f_alerts_event("SetAlertFailed", token);
			}
		}},
		{{"Alerts", "DeleteAlert"}, [this](auto a_directive)
		{
			auto& payload = a_directive / "directive" / "payload";
			auto token = payload / "token"_jss;
			try {
				this->f_alerts_delete(token);
				if (v_alerts_changed) v_alerts_changed();
				this->f_alerts_event("DeleteAlertSucceeded", token);
			} catch (std::exception& e) {
				this->f_exception_encountered("Alerts.DeleteAlert", "INTERNAL_ERROR", e.what());
				this->f_alerts_event("DeleteAlertFailed", token);
			}
		}},
		{{"AudioPlayer", "Play"}, [this](const picojson::value& a_directive)
		{
			auto& payload = a_directive / "directive" / "payload";
			auto behavior = payload / "playBehavior"_jss;
			if (behavior == "REPLACE_ALL") {
				v_content->f_clear();
				v_content->f_stop();
			} else if (behavior == "REPLACE_ENQUEUED") {
				v_content->f_clear();
			}
			auto& stream = payload / "audioItem" / "stream";
			auto url = stream / "url"_jss;
			auto token = stream / "token"_jss;
			if (v_log) v_log(e_severity__TRACE) << "queuing: " << url << ", " << token << std::endl;
			std::function<t_audio_source*()> open;
			if (url.substr(0, 4) == "cid:")
				open = [this, token, audio = std::make_shared<t_attached_audio>(*this, v_content->v_task, url.substr(4))]
				{
					return audio->f_open([this]
					{
						v_content_stuttering = std::chrono::steady_clock::now();
						this->f_player_event("PlaybackStutterStarted");
					}, [this, token]
					{
						this->f_event(this->f_metadata("AudioPlayer", "PlaybackStutterFinished", {
							{"token", picojson::value(token)},
							{"offsetInMilliseconds", picojson::value(static_cast<double>(v_content->f_offset()))},
							{"stutterDurationInMilliseconds", picojson::value(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - v_content_stuttering).count()))}
						}));
						v_content_stuttering = std::chrono::steady_clock::time_point();
					});
				};
			else
				open = [this, url]
				{
					std::thread([&]
					{
						try {
							v_scheduler.dispatch([&, source = v_open_audio_by_url(url)]
							{
								v_content->v_task.f_post([source](auto)
								{
									throw source;
								});
							});
						} catch (...) {
							v_scheduler.dispatch([&, e = std::current_exception()]
							{
								v_content->v_task.f_post([e](auto)
								{
									std::rethrow_exception(e);
								});
							});
						}
					}).detach();
					try {
						while (true) v_content->v_task.f_wait();
					} catch (t_audio_source* source) {
						return source;
					}
				};
			v_content->f_queue([this, token, open = std::move(open)]
			{
				v_content->v_target.f_reset();
				v_content->v_playing = token;
				this->f_player_event("PlaybackStarted");
				if (v_state_changed) v_state_changed();
				try {
					std::unique_ptr<t_audio_source> source(open());
					t_audio_decoder decoder(*source);
					v_content->f_loop(decoder);
					this->f_player_event("PlaybackNearlyFinished");
					v_content->f_flush();
					this->f_player_event("PlaybackFinished");
				} catch (nullptr_t) {
					this->f_player_event("PlaybackStopped");
				} catch (std::exception& e) {
					this->f_event(this->f_metadata("AudioPlayer", "PlaybackFailed", {
						{"token", picojson::value(token)},
						{"currentPlaybackState", picojson::value(picojson::value::object{
							{"token", picojson::value(token)},
							{"offsetInMilliseconds", picojson::value(static_cast<double>(v_content->f_offset()))},
							{"playerActivity", picojson::value("PLAYING")}
						})},
						{"error", picojson::value(picojson::value::object{
							{"type", picojson::value("MEDIA_ERROR_UNKNOWN")},
							{"message", picojson::value(e.what())}
						})}
					}));
				}
				v_content->v_playing.clear();
				if (v_state_changed) v_state_changed();
			});
			auto report = stream * "progressReport";
			if (!report) return;
			auto delay = *report * "progressReportDelayInMilliseconds";
			if (!!delay) v_scheduler.f_run_in(std::chrono::milliseconds(static_cast<long>((*delay).get<double>())), [this](auto)
			{
				this->f_player_event("ProgressReportDelayElapsed");
			});
			auto interval = *report * "progressReportIntervalInMilliseconds";
			if (!!interval) v_scheduler.f_run_every(std::chrono::milliseconds(static_cast<long>((*interval).get<double>())), [this, token](auto)
			{
				if (v_content->v_playing != token) return false;
				this->f_player_event("ProgressReportIntervalElapsed");
				return true;
			});
		}},
		{{"AudioPlayer", "Stop"}, [this](auto)
		{
			v_content->f_stop();
		}},
		{{"AudioPlayer", "ClearQueue"}, [this](auto a_directive)
		{
			auto& payload = a_directive / "directive" / "payload";
			auto behavior = payload / "clearBehavior"_jss;
			if (behavior == "CLEAR_ENQUEUED") {
				v_content->f_clear();
			} else if (behavior == "CLEAR_ALL") {
				v_content->f_clear();
				v_content->f_stop();
			}
			this->f_empty_event("AudioPlayer", "PlaybackQueueCleared");
		}},
		{{"Speaker", "SetVolume"}, [this](auto a_directive)
		{
			f_speaker_volume(a_directive / "directive" / "payload" / "volume"_jsn);
		}},
		{{"Speaker", "AdjustVolume"}, [this](auto a_directive)
		{
			f_speaker_volume(v_speaker_volume + a_directive / "directive" / "payload" / "volume"_jsn);
		}},
		{{"Speaker", "SetMute"}, [this](auto a_directive)
		{
			f_speaker_muted(a_directive / "directive" / "payload" / "mute"_jsb);
		}},
		{{"SpeechSynthesizer", "Speak"}, [this](auto a_directive)
		{
			auto& payload = a_directive / "directive" / "payload";
			v_dialog->f_queue([this, token = payload / "token"_jss, audio = std::make_shared<t_attached_audio>(*this, v_dialog->v_task, (payload / "url"_jss).substr(4))]
			{
				auto f = [this, token](const std::string& a_name)
				{
					this->f_event(this->f_metadata("SpeechSynthesizer", a_name, {
						{"token", picojson::value(token)}
					}));
				};
				this->f_dialog_acquire(v_dialog->v_task);
				v_dialog->v_target.f_reset();
				v_dialog->v_playing = token;
				f("SpeechStarted");
				if (v_state_changed) v_state_changed();
				try {
					std::unique_ptr<t_audio_source> source(audio->f_open([] {}, [] {}));
					t_audio_decoder decoder(*source);
					v_dialog->f_loop(decoder);
					v_dialog->f_flush();
				} catch (nullptr_t) {
					this->f_exception_encountered("SpeechSynthesizer.Speak", "INTERNAL_ERROR", "Stopped");
				} catch (std::exception& e) {
					this->f_exception_encountered("SpeechSynthesizer.Speak", "INTERNAL_ERROR", e.what());
				}
				f("SpeechFinished");
				v_dialog->v_playing.clear();
				this->f_dialog_release();
				if (v_state_changed) v_state_changed();
			});
		}},
		{{"System", "ResetUserInactivity"}, [this](auto)
		{
			v_last_activity = std::chrono::steady_clock::now();
		}}
	};
	std::map<std::string, t_attached_audio*> v_id2audio;
	t_channel* v_dialog = nullptr;
	bool v_dialog_active = false;
	std::map<std::string, t_alert> v_alerts;
	size_t v_alerts_duration = 60;
	t_channel* v_content = nullptr;
	bool v_content_can_play_in_background = false;
	bool v_content_pausing = false;
	std::chrono::steady_clock::time_point v_content_stuttering;
	long v_speaker_volume = 100;
	bool v_speaker_muted = false;
	t_task* v_recognizer = nullptr;
	size_t v_capture_threshold = 32 * 1024;
	size_t v_capture_integral = 0;
	std::chrono::steady_clock::time_point v_capture_exceeded;
	bool v_capture_busy = false;
	bool v_capture_auto = true;
	bool v_capture_force = false;
	std::function<void()> v_expecting_speech;
	boost::asio::steady_timer* v_expecting_timeout = nullptr;
	std::string v_expecting_dialog_id;
	std::chrono::steady_clock::time_point v_last_activity;

	void f_reconnect()
	{
		f_disconnect();
		if (v_log) v_log(e_severity__INFORMATION) << "reconnect in " << v_reconnecting_interval << " seconds." << std::endl;
		v_reconnecting = &v_scheduler.f_run_in(std::chrono::seconds(v_reconnecting_interval), [this](auto a_ec)
		{
			if (v_reconnecting) {
				v_reconnecting = nullptr;
				this->f_connect();
			} else {
				v_reconnecting_interval = 1;
			}
		});
		if (v_reconnecting_interval < 256) v_reconnecting_interval *= 2;
	}
	picojson::value f_context() const
	{
		picojson::value::array all;
		picojson::value::array active;
		for (auto& x : v_alerts) {
			picojson::value alert(picojson::value::object{
				{"token", picojson::value(x.first)},
				{"type", picojson::value(x.second.v_type)},
				{"scheduledTime", picojson::value(x.second.v_at)}
			});
			if (x.second.v_source) active.push_back(alert);
			all.push_back(std::move(alert));
		}
		return picojson::value(picojson::value::array{
			picojson::value(picojson::value::object{
				{"header", picojson::value(picojson::value::object{
					{"namespace", picojson::value("AudioPlayer")},
					{"name", picojson::value("PlaybackState")}
				})},
				{"payload", picojson::value(picojson::value::object{
					{"token", picojson::value(v_content->v_playing)},
					{"offsetInMilliseconds", picojson::value(static_cast<double>(v_content->v_playing.empty() ? 0 : v_content->f_offset()))},
					{"playerActivity", picojson::value(v_content->v_playing.empty() ? (v_content->f_offset() > 0 ? "FINISHED" : "IDLE") : v_content_pausing ? "STOPPED" : v_content_stuttering > std::chrono::steady_clock::time_point() ? "BUFFER_UNDERRUN" : "PLAYING")}
				})}
			}),
			picojson::value(picojson::value::object{
				{"header", picojson::value(picojson::value::object{
					{"namespace", picojson::value("Alerts")},
					{"name", picojson::value("AlertsState")}
				})},
				{"payload", picojson::value(picojson::value::object{
					{"allAlerts", picojson::value(all)},
					{"activeAlerts", picojson::value(active)}
				})}
			}),
			picojson::value(picojson::value::object{
				{"header", picojson::value(picojson::value::object{
					{"namespace", picojson::value("Speaker")},
					{"name", picojson::value("VolumeState")}
				})},
				{"payload", picojson::value(picojson::value::object{
					{"volume", picojson::value(static_cast<double>(v_speaker_volume))},
					{"muted", picojson::value(v_speaker_muted)}
				})}
			}),
			picojson::value(picojson::value::object{
				{"header", picojson::value(picojson::value::object{
					{"namespace", picojson::value("SpeechSynthesizer")},
					{"name", picojson::value("SpeechState")}
				})},
				{"payload", picojson::value(picojson::value::object{
					{"token", picojson::value(v_dialog->v_playing)},
					{"offsetInMilliseconds", picojson::value(static_cast<double>(v_dialog->v_playing.empty() ? 0 : v_dialog->f_offset()))},
					{"playerActivity", picojson::value(v_dialog->v_playing.empty() ? "FINISHED" : "PLAYING")}
				})}
			})
		});
	}
	picojson::value f_metadata(const std::string& a_namespace, const std::string& a_name, picojson::value::object&& a_payload)
	{
		if (v_log) v_log(e_severity__INFORMATION) << "event: " << a_namespace << "." << a_name << std::endl;
		return picojson::value(picojson::value::object{
			{"event", picojson::value(picojson::value::object{
				{"header", picojson::value(picojson::value::object{
					{"namespace", picojson::value(a_namespace)},
					{"name", picojson::value(a_name)},
					{"messageId", picojson::value("messateId-" + std::to_string(++v_message_id))}
				})},
				{"payload", picojson::value(std::move(a_payload))}
			})}
		});
	}
	void f_setup(const nghttp2::asio_http2::client::response& a_response)
	{
		if (v_log) {
			auto& log = v_log(e_severity__TRACE);
			for (auto& x : a_response.header()) log << x.first << ": " << x.second.value << std::endl;
		}
		auto i = a_response.header().find("content-type");
		if (i == a_response.header().end()) return;
		std::smatch match;
		if (!std::regex_match(i->second.value, match, v_re_content_type)) return;
		a_response.on_data([this, &a_response, parser = std::make_shared<t_parser>(*this, match[1].str())](auto a_p, size_t a_n)
		{
			if (v_log) v_log(e_severity__TRACE) << "response(" << &a_response << ") on data: " << a_n << std::endl;
			(*parser)(a_p, a_n);
		});
	}
	template<typename T_data>
	const nghttp2::asio_http2::client::request* f_post(T_data a_data)
	{
		if (!v_online) {
			if (v_log) v_log(e_severity__INFORMATION) << "offline." << std::endl;
			return nullptr;
		}
		boost::system::error_code ec;
		auto request = v_session->submit(ec, "POST", "https://avs-alexa-na.amazon.com/v20160207/events", a_data, v_header);
		if (!request) {
			if (v_log) v_log(e_severity__ERROR) << "events POST failed: " << ec.message() << std::endl;
			f_reconnect();
			return nullptr;
		}
		if (v_log) v_log(e_severity__TRACE) << "events POST(" << request << ") opened." << std::endl;
		request->on_response([this, request](auto& a_response)
		{
			if (v_log) v_log(e_severity__TRACE) << "events POST(" << request << ") on response(" << &a_response << ") " << a_response.status_code() << std::endl;
			this->f_setup(a_response);
		});
		return request;
	}
	void f_event(const picojson::value& a_metadata)
	{
		auto request = f_post(v_boundary_metadata + a_metadata.serialize() + v_boundary_terminator);
		if (request) request->on_close([this, request](auto a_code)
		{
			if (v_log) v_log(e_severity__TRACE) << "events POST(" << request << ") on close: " << a_code << std::endl;
		});
	}
	void f_empty_event(const std::string& a_namespace, const std::string& a_name)
	{
		f_event(f_metadata(a_namespace, a_name, {}));
	}
	void f_exception_encountered(const std::string& a_directive, const std::string& a_type, const char* a_message)
	{
		auto metadata = f_metadata("System", "ExceptionEncountered", {
			{"unparsedDirective", picojson::value(a_directive)},
			{"error", picojson::value(picojson::value::object{
				{"type", picojson::value(a_type)},
				{"message", picojson::value(a_message)}
			})}
		});
		metadata << "context" & f_context();
		f_event(metadata);
	}
	void f_alerts_event(const std::string& a_name, const std::string& a_token)
	{
		f_event(f_metadata("Alerts", a_name, {
			{"token", picojson::value(a_token)}
		}));
	}
protected:
	void f_alerts_set(const std::string& a_token, const std::string& a_type, const std::string& a_at)
	{
		std::tm tm{};
		std::stringstream s(a_at);
		s >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
		if (s.fail()) throw std::runtime_error("invalid format");
		auto t = std::mktime(&tm);
		tzset();
		t -= timezone;
		if (t == -1) throw std::runtime_error("invalid time");
		auto at = std::chrono::system_clock::from_time_t(t);
		if (at < std::chrono::system_clock::now() - std::chrono::minutes(30)) {
			f_alerts_event("AlertStopped", a_token);
			return;
		}
		auto i = v_alerts.emplace(a_token, t_alert(a_type, a_at)).first;
		i->second.v_timer.reset(new boost::asio::system_timer(v_scheduler.f_io(), at));
		i->second.v_timer->async_wait(v_scheduler.wrap([this, i](auto a_ec)
		{
			if (a_ec == boost::asio::error::operation_aborted) {
				v_alerts.erase(i);
				if (v_alerts_changed) v_alerts_changed();
				return;
			}
			if (v_log) v_log(e_severity__INFORMATION) << "alert: " << i->first << std::endl;
			auto f = [this, i]
			{
				i->second.f_play(v_sounds[(i->second.v_type == "TIMER" ? 0 : 2) + (v_dialog_active ? 1 : 0)], v_dialog_active ? 1.0f / 16.0f : 1.0f);
				this->f_alerts_event("AlertStarted", i->first);
				i->second.v_timer->expires_from_now(std::chrono::seconds(v_alerts_duration));
				i->second.v_timer->async_wait(v_scheduler.wrap([this, i](auto)
				{
					this->f_alerts_event("AlertStopped", i->first);
					v_alerts.erase(i);
					if (v_alerts_changed) v_alerts_changed();
					if (v_dialog_active) return;
					for (auto& x : v_alerts) if (x.second.v_source) return;
					this->f_player_foreground();
				}));
				if (v_state_changed) v_state_changed();
			};
			if (v_dialog_active)
				f();
			else
				this->f_player_background(f);
		}));
	}
private:
	void f_alerts_delete(const std::string& a_token)
	{
		auto& alert = v_alerts.at(a_token);
		if (alert.v_source) throw std::runtime_error("already active");
		alert.v_timer->cancel();
	}
	void f_player_event(const std::string& a_name)
	{
		f_event(f_metadata("AudioPlayer", a_name, {
			{"token", picojson::value(v_content->v_playing)},
			{"offsetInMilliseconds", picojson::value(static_cast<double>(v_content->f_offset()))}
		}));
	}
	template<typename T_done>
	void f_player_background(T_done a_done)
	{
		if (v_content_can_play_in_background) {
			alSourcef(v_content->v_target, AL_GAIN, 1.0f / 16.0f);
			a_done();
		} else if (v_content_pausing) {
			a_done();
		} else {
			v_content->v_task.f_post([this, a_done](auto)
			{
				if (!v_content->v_playing.empty()) {
					alSourcePause(v_content->v_target);
					this->f_player_event("PlaybackPaused");
				}
				v_content_pausing = true;
				a_done();
				do v_content->v_task.f_wait(); while (v_content_pausing);
				if (!v_content->v_playing.empty()) {
					alSourcePlay(v_content->v_target);
					this->f_player_event("PlaybackResumed");
				}
			});
		}
	}
	void f_player_foreground()
	{
		if (v_content_can_play_in_background) {
			alSourcef(v_content->v_target, AL_GAIN, 1.0f);
		} else {
			v_content_pausing = false;
			v_content->v_task.f_notify();
		}
	}
	void f_playback_event(const std::string& a_name)
	{
		alSourcePause(v_content->v_target);
		auto metadata = f_metadata("PlaybackController", a_name, {});
		metadata << "context" & f_context();
		f_event(metadata);
	}
	void f_speaker_event(const std::string& a_name)
	{
		f_event(f_metadata("Speaker", a_name, {
			{"volume", picojson::value(static_cast<double>(v_speaker_volume))},
			{"muted", picojson::value(v_speaker_muted)}
		}));
	}
	void f_speaker_apply()
	{
		alListenerf(AL_GAIN, v_speaker_muted ? 0.0f : v_speaker_volume / 100.0f);
		if (v_speaker_changed) v_speaker_changed();
	}
	void f_dialog_acquire(t_task& a_task)
	{
		while (v_dialog_active) a_task.f_wait();
		v_dialog_active = true;
		for (auto& x : v_alerts) {
			if (!x.second.v_source) continue;
			x.second.f_play(v_sounds[x.second.v_type == "TIMER" ? 1 : 3], 1.0f / 16.0f);
			f_alerts_event("AlertEnteredBackground", x.first);
		}
		bool done = false;
		f_player_background([&]
		{
			done = true;
			a_task.f_notify();
		});
		while (!done) a_task.f_wait();
	}
	void f_dialog_release()
	{
		bool b = false;
		for (auto& x : v_alerts) {
			if (!x.second.v_source) continue;
			x.second.f_play(v_sounds[x.second.v_type == "TIMER" ? 0 : 2], 1.0f);
			f_alerts_event("AlertEnteredForeground", x.first);
			b = true;
		}
		if (!b) f_player_foreground();
		v_dialog_active = false;
		v_dialog->v_task.f_notify();
		v_recognizer->f_notify();
	}
	bool f_capture(ALCdevice* a_device, char* a_buffer, bool a_busy)
	{
		while (true) {
			if (!a_busy && !v_expecting_timeout && v_expecting_speech) v_expecting_speech();
			if ((v_capture_busy && v_capture_auto && v_dialog->v_playing.empty() && v_content->v_playing.empty() || v_capture_force) != a_busy) return false;
			ALCint n;
			alcGetIntegerv(a_device, ALC_CAPTURE_SAMPLES, 1, &n);
			if (n >= 160) break;
			v_recognizer->f_wait(std::chrono::milliseconds((160 - n) / 16));
		}
		alcCaptureSamples(a_device, a_buffer, 160);
		v_capture_integral /= 2;
		for (size_t i = 0; i < 160; ++i) v_capture_integral += std::abs(reinterpret_cast<int16_t*>(a_buffer)[i]);
		auto now = std::chrono::steady_clock::now();
		if (v_capture_integral > v_capture_threshold) v_capture_exceeded = now;
		v_capture_busy = now - v_capture_exceeded < std::chrono::seconds(1);
		if (v_capture) v_capture();
		return true;
	}
	void f_recognizer()
	{
		while (true) {
			std::unique_ptr<ALCdevice, decltype(&alcCaptureCloseDevice)> device(alcCaptureOpenDevice(NULL, 16000, AL_FORMAT_MONO16, 1600), alcCaptureCloseDevice);
			if (!device) {
				if (v_log) v_log(e_severity__ERROR) << "alcCaptureOpenDevice: " << alGetError() << std::endl;
				v_recognizer->f_wait(std::chrono::seconds(5));
				continue;
			}
			alcCaptureStart(device.get());
			auto queue = std::make_shared<std::pair<std::deque<char>, bool>>(std::deque<char>(), false);
			auto& deque = queue->first;
			char buffer[320];
			const size_t window = sizeof(buffer) * 100;
			while (f_capture(device.get(), buffer, false)) {
				size_t n = deque.size() + sizeof(buffer);
				if (n > window) deque.erase(deque.begin(), deque.begin() + (n - window));
				deque.insert(deque.end(), buffer, buffer + sizeof(buffer));
			}
			if (v_expecting_timeout) {
				v_expecting_timeout->cancel();
				v_expecting_speech = nullptr;
				v_expecting_timeout = nullptr;
			} else {
				if (v_capture_force) {
					v_dialog->f_clear();
					v_dialog->f_stop();
				}
				f_dialog_acquire(*v_recognizer);
			}
			if (v_state_changed) v_state_changed();
			if (v_log) v_log(e_severity__INFORMATION) << "recognize started." << std::endl;
			deque.insert(deque.begin(), v_boundary_audio.begin(), v_boundary_audio.end());
			{
				auto metadata = f_metadata("SpeechRecognizer", "Recognize", {
					{"profile", picojson::value("CLOSE_TALK")},
					{"format", picojson::value("AUDIO_L16_RATE_16000_CHANNELS_1")}
				});
				metadata << "context" & f_context();
				if (v_expecting_dialog_id.empty()) {
					metadata / "event" / "header" << "dialogRequestId" & "dialogRequestId-" + std::to_string(++v_dialog_id);
				} else {
					metadata / "event" / "header" << "dialogRequestId" & v_expecting_dialog_id;
					v_expecting_dialog_id.clear();
				}
				auto s = metadata.serialize();
				deque.insert(deque.begin(), s.begin(), s.end());
			}
			deque.insert(deque.begin(), v_boundary_metadata.begin(), v_boundary_metadata.end());
			auto request = f_post([queue](auto a_p, auto a_n, auto a_flags)
			{
				auto& deque = queue->first;
				if (a_n > deque.size()) a_n = deque.size();
				if (a_n > 0) {
					auto i = deque.begin();
					std::copy_n(i, a_n, a_p);
					deque.erase(i, i + a_n);
				} else {
					if (!queue->second) return static_cast<size_t>(NGHTTP2_ERR_DEFERRED);
					*a_flags |= NGHTTP2_DATA_FLAG_EOF;
				}
				return a_n;
			});
			if (request) {
				auto p = std::make_shared<decltype(request)>(request);
				request->on_close([this, p](auto a_code)
				{
					if (v_log) v_log(e_severity__TRACE) << "events POST(" << *p << ") on close: " << a_code << std::endl;
					*p = nullptr;
					v_recognizer->f_notify();
				});
				while (f_capture(device.get(), buffer, true)) {
					deque.insert(deque.end(), buffer, buffer + 320);
					if (*p) request->resume();
				}
				f_dialog_release();
				if (*p) {
					deque.insert(deque.end(), v_boundary_terminator.begin(), v_boundary_terminator.end());
					queue->second = true;
					request->resume();
					device.reset();
					do v_recognizer->f_wait(); while (*p);
				}
				if (v_log) v_log(e_severity__INFORMATION) << "recognize finished." << std::endl;
			} else {
				if (v_log) v_log(e_severity__ERROR) << "recognize failed." << std::endl;
				while (f_capture(device.get(), buffer, true));
				f_dialog_release();
			}
			v_last_activity = std::chrono::steady_clock::now();
			if (v_state_changed) v_state_changed();
		}
	}

public:
	std::function<std::ostream&(t_severity)> v_log;
	std::function<void()> v_capture;
	std::function<void()> v_state_changed;
	std::function<void()> v_options_changed;
	std::function<void()> v_alerts_changed;
	std::function<void()> v_speaker_changed;
	std::function<t_audio_source*(const std::string&)> v_open_audio_by_url = [](auto& a_url)
	{
		return new t_url_audio_source(a_url.c_str());
	};

	t_session(t_scheduler& a_scheduler, const std::function<std::ostream&(t_severity)>& a_log, const ALuint* a_sounds) : v_tls(boost::asio::ssl::context::tlsv12), v_scheduler(a_scheduler), v_log(a_log), v_sounds(a_sounds)
	{
		v_tls.set_default_verify_paths();
		boost::system::error_code ec;
		nghttp2::asio_http2::client::configure_tls_context(ec, v_tls);
		auto run = [this](const char* a_name, auto a_run)
		{
			while (true) {
				try {
					a_run();
				} catch (t_scheduler::t_stop&) {
					throw;
				} catch (std::exception& e) {
					if (v_log) v_log(e_severity__ERROR) << a_name << ": caught " << e.what() << std::endl;
				} catch (...) {
					if (v_log) v_log(e_severity__ERROR) << a_name << ": caught unknown" << std::endl;
				}
			}
		};
		v_scheduler.f_spawn([this, run](auto& a_task)
		{
			t_channel dialog(a_task);
			v_dialog = &dialog;
			run("dialog", [this]
			{
				v_dialog->f_run();
			});
		});
		v_scheduler.f_spawn([this, run](auto& a_task)
		{
			t_channel content(a_task);
			v_content = &content;
			run("content", [this]
			{
				v_content->f_run();
			});
		});
		v_scheduler.f_spawn([this, run](auto& a_task)
		{
			v_recognizer = &a_task;
			run("recognizer", [this]
			{
				this->f_recognizer();
			});
		});
		v_scheduler.f_run_every(std::chrono::hours(1), [this](auto)
		{
			this->f_event(this->f_metadata("System", "UserInactivityReport", {
				{"inactiveTimeInSeconds", picojson::value(static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - v_last_activity).count()))}
			}));
			return true;
		});
	}
	t_scheduler& f_scheduler() const
	{
		return v_scheduler;
	}
	bool f_online() const
	{
		return v_online;
	}
	void f_connect()
	{
		if (v_reconnecting) {
			v_reconnecting->cancel();
			v_reconnecting = nullptr;
		}
		if (v_session) return;
		v_session.reset(new nghttp2::asio_http2::client::session(v_scheduler.f_io(), v_tls, "avs-alexa-na.amazon.com", "https"));
		v_session->read_timeout(boost::posix_time::hours(1));
		if (v_log) v_log(e_severity__TRACE) << "session(" << v_session.get() << ") created." << std::endl;
		v_session->on_connect([this](auto)
		{
			boost::system::error_code ec;
			auto request = v_session->submit(ec, "GET", "https://avs-alexa-na.amazon.com/v20160207/directives", v_header);
			if (!request) {
				if (v_log) v_log(e_severity__ERROR) << "directives GET failed: " << ec.message() << std::endl;
				this->f_reconnect();
				return;
			}
			if (v_log) v_log(e_severity__TRACE) << "directives GET(" << request << ") opened." << std::endl;
			request->on_response([this, request](auto& a_response)
			{
				if (v_log) v_log(e_severity__TRACE) << "directives GET(" << request << ") on response(" << &a_response << ") " << a_response.status_code() << std::endl;
				this->f_setup(a_response);
				v_online = true;
				v_reconnecting_interval = 1;
				auto metadata = this->f_metadata("System", "SynchronizeState", {});
				metadata << "context" & this->f_context();
				this->f_event(metadata);
				if (v_state_changed) v_state_changed();
			});
			request->on_close([this, request](auto a_code)
			{
				if (v_log) v_log(e_severity__TRACE) << "directives GET(" << request << ") closed: " << a_code << std::endl;
			});
		});
		v_session->on_error([this](auto a_ec)
		{
			if (v_log) v_log(e_severity__ERROR) << "session(" << v_session.get() << ") on error: " << a_ec.message() << std::endl;
			this->f_reconnect();
		});
	}
	void f_disconnect()
	{
		if (v_reconnecting) {
			v_reconnecting->cancel();
			v_reconnecting = nullptr;
		}
		v_online = false;
		if (v_session) {
			v_session->shutdown();
			v_session.reset();
		}
		if (v_state_changed) v_state_changed();
	}
	void f_token(const std::string& a_token)
	{
		v_header = nghttp2::asio_http2::header_map{
			{"authorization", {"Bearer " + a_token, true}},
			{"content-type", {"multipart/form-data; boundary=this-is-a-boundary", true}}
		};
		f_connect();
	}
	bool f_dialog_active() const
	{
		return v_dialog_active;
	}
	bool f_dialog_playing() const
	{
		return !v_dialog->v_playing.empty();
	}
	const std::map<std::string, t_alert>& f_alerts() const
	{
		return v_alerts;
	}
	void f_alerts_stop(const std::string& a_token)
	{
		auto i = v_alerts.find(a_token);
		if (i != v_alerts.end() && i->second.v_source) i->second.v_timer->cancel();
	}
	size_t f_alerts_duration() const
	{
		return v_alerts_duration;
	}
	void f_alerts_duration(size_t a_value)
	{
		if (a_value == v_alerts_duration) return;
		v_alerts_duration = a_value;
		if (v_options_changed) v_options_changed();
	}
	bool f_content_can_play_in_background() const
	{
		return v_content_can_play_in_background;
	}
	void f_content_can_play_in_background(bool a_value)
	{
		if (a_value == v_content_can_play_in_background) return;
		v_content_can_play_in_background = a_value;
		if (v_options_changed) v_options_changed();
	}
	bool f_content_playing() const
	{
		return !v_content->v_playing.empty();
	}
	long f_speaker_volume() const
	{
		return v_speaker_volume;
	}
	void f_speaker_volume(long a_volume)
	{
		if (a_volume < 0) a_volume = 0;
		if (a_volume > 100) a_volume = 100;
		v_speaker_volume = a_volume;
		f_speaker_apply();
		f_speaker_event("VolumeChanged");
	}
	bool f_speaker_muted() const
	{
		return v_speaker_muted;
	}
	void f_speaker_muted(bool a_muted)
	{
		v_speaker_muted = a_muted;
		f_speaker_apply();
		f_speaker_event("MuteChanged");
	}
	void f_playback_play()
	{
		f_playback_event("PlayCommandIssued");
	}
	void f_playback_pause()
	{
		f_playback_event("PauseCommandIssued");
	}
	void f_playback_next()
	{
		f_playback_event("NextCommandIssued");
	}
	void f_playback_previous()
	{
		f_playback_event("PreviousCommandIssued");
	}
	size_t f_capture_threshold() const
	{
		return v_capture_threshold;
	}
	void f_capture_threshold(size_t a_value)
	{
		if (a_value == v_capture_threshold) return;
		v_capture_threshold = a_value;
		v_recognizer->f_notify();
		if (v_options_changed) v_options_changed();
	}
	size_t f_capture_integral() const
	{
		return v_capture_integral;
	}
	bool f_capture_busy() const
	{
		return v_capture_busy;
	}
	bool f_capture_auto() const
	{
		return v_capture_auto;
	}
	void f_capture_auto(bool a_value)
	{
		if (a_value == v_capture_auto) return;
		v_capture_auto = a_value;
		v_recognizer->f_notify();
		if (v_options_changed) v_options_changed();
	}
	bool f_capture_force() const
	{
		return v_capture_force;
	}
	void f_capture_force(bool a_value)
	{
		if (a_value == v_capture_force) return;
		v_capture_force = a_value;
		v_recognizer->f_notify();
		if (v_options_changed) v_options_changed();
	}
	bool f_expecting_speech() const
	{
		return static_cast<bool>(v_expecting_speech);
	}
};

#endif
