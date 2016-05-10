#include <deque>
#include <cstdio>
#include <boost/asio/system_timer.hpp>
#include <nghttp2/asio_http2_server.h>
#include <nghttp2/asio_http2_client.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "multipart.h"
#include "decoder.h"
#include "scheduler.h"
#include "tiny_http.h"

std::string f_to_json(const boost::property_tree::ptree& a_tree, bool a_pretty)
{
	std::stringstream s;
	boost::property_tree::write_json(s, a_tree, a_pretty);
	return s.str();
}

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
		void f_run()
		{
			while (true) {
				while (v_directives.empty()) v_task.f_wait();
				v_directives.front()();
				v_directives.pop_front();
			}
		}
		void f_loop(t_audio_decoder& a_decoder)
		{
			a_decoder([this](size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
			{
				v_target(a_channels, a_bytes, a_p, a_n, a_rate);
				if (v_target.f_remain() > 2.0) v_task.f_wait(std::chrono::seconds(1));
			});
		}
		void f_flush()
		{
			while (v_target.f_flush() > 0) v_task.f_wait(std::chrono::milliseconds(static_cast<int>(v_target.f_remain() * 250.0)));
		}
	};
	struct t_alert
	{
		std::string v_type;
		std::string v_at;
		std::unique_ptr<boost::asio::system_timer> v_timer;
		bool v_active = false;
	};
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
			boost::property_tree::ptree directive;
			{
				std::istream s(&v_metadata);
				boost::property_tree::read_json(s, directive);
			}
#ifdef ALEXAAGENT_LOG
			std::fprintf(stderr, "json: %s\n", f_to_json(directive, true).c_str());
#endif
			auto ns = directive.get<std::string>("directive.header.namespace");
			auto name = directive.get<std::string>("directive.header.name");
			std::fprintf(stderr, "parser(%p) directive: %s.%s\n", this, ns.c_str(), name.c_str());
			auto i = v_session.v_handlers.find(std::make_pair(ns, name));
			if (i == v_session.v_handlers.end())
				std::fprintf(stderr, "parser(%p) unknown directive: %s.%s\n", this, ns.c_str(), name.c_str());
			else
				i->second(directive);
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
			std::fprintf(stderr, "parser(%p) part: %s, %s\n", this, a_type.c_str(), a_id.c_str());
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

	static std::string v_boundary_metadata;
	static std::string v_boundary_audio;
	static std::string v_boundary_terminator;
	static std::regex v_re_content_type;

	boost::asio::ssl::context v_tls;
	t_scheduler& v_scheduler;
	nghttp2::asio_http2::header_map v_header;
	std::unique_ptr<nghttp2::asio_http2::client::session> v_session;
	bool v_ready = false;
	size_t v_message_id = 0;
	size_t v_dialog_id = 0;
	std::map<std::pair<std::string, std::string>, std::function<void(const boost::property_tree::ptree&)>> v_handlers{
		{std::make_pair("SpeechRecognizer", "ExpectSpeech"), [this](const boost::property_tree::ptree& a_directive)
		{
			v_dialog->f_queue([this, dialog_id = a_directive.get("directive.header.dialogRequestId", std::string()), timeout = a_directive.get<long>("directive.payload.timeoutInMilliseconds")]
			{
				v_expecting_dialog_id = dialog_id;
				v_expecting_speech = [this, timeout]
				{
					this->f_dialog_acquire(*v_recognizer);
					std::fprintf(stderr, "expecting speech within %dms.\n", timeout);
					v_expecting_timeout = &v_scheduler.f_run_in(std::chrono::milliseconds(timeout), [this](auto a_ec)
					{
						if (a_ec == boost::asio::error::operation_aborted) return;
						v_expecting_speech = nullptr;
						v_expecting_timeout = nullptr;
						this->f_empty_event("SpeechRecognizer", "ExpectSpeechTimedOut");
						this->f_dialog_release();
					});
				};
				do v_dialog->v_task.f_wait(); while (v_expecting_speech);
			});
		}},
		{std::make_pair("Alerts", "SetAlert"), [this](const boost::property_tree::ptree& a_directive)
		{
			auto token = a_directive.get<std::string>("directive.payload.token");
			try {
				this->f_alerts_set(token, a_directive.get<std::string>("directive.payload.type"), a_directive.get<std::string>("directive.payload.scheduledTime"));
				this->f_alerts_save();
				this->f_alerts_event("SetAlertSucceeded", token);
			} catch (std::exception&) {
				this->f_alerts_event("SetAlertFailed", token);
			}
		}},
		{std::make_pair("AudioPlayer", "Play"), [this](const boost::property_tree::ptree& a_directive)
		{
			auto behavior = a_directive.get<std::string>("directive.payload.playBehavior");
			if (behavior == "REPLACE_ALL") {
				v_content->v_directives.clear();
				this->f_player_stop();
			} else if (behavior == "REPLACE_ENQUEUED") {
				v_content->v_directives.clear();
			}
			auto url = a_directive.get<std::string>("directive.payload.audioItem.stream.url");
			auto token = a_directive.get<std::string>("directive.payload.audioItem.stream.token");
			std::fprintf(stderr, "queuing: %s, %s\n", url.c_str(), token.c_str());
			std::function<t_audio_source*()> open;
			if (url.substr(0, 4) == "cid:")
				open = [this, token, audio = std::make_shared<t_attached_audio>(*this, v_content->v_task, url.substr(4))]
				{
					return audio->f_open([this]
					{
						v_stuttering = std::chrono::steady_clock::now();
						this->f_player_event("PlaybackStutterStarted");
					}, [this, token]
					{
						boost::property_tree::ptree metadata;
						this->f_metadata("AudioPlayer", "PlaybackStutterFinished", metadata);
						metadata.put("event.payload.token", token);
						metadata.put("event.payload.offsetInMilliseconds", v_content->f_offset());
						metadata.put("event.payload.stutterDurationInMilliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - v_stuttering).count());
						this->f_event(metadata);
						v_stuttering = std::chrono::steady_clock::time_point();
					});
				};
			else
				open = [url]
				{
					return new t_url_audio_source(url.c_str());
				};
			v_content->f_queue([this, token, open = std::move(open)]
			{
				v_content->v_target.f_reset();
				v_content->v_playing = token;
				this->f_player_event("PlaybackStarted");
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
					boost::property_tree::ptree metadata;
					this->f_metadata("AudioPlayer", "PlaybackFailed", metadata);
					metadata.put("event.payload.token", token);
					metadata.put("event.payload.currentPlaybackState.token", token);
					metadata.put("event.payload.currentPlaybackState.offsetInMilliseconds", v_content->f_offset());
					metadata.put("event.payload.currentPlaybackState.playerActivity", "PLAYING");
					metadata.put("event.payload.error.type", "MEDIA_ERROR_UNKNOWN");
					metadata.put("event.payload.error.message", e.what());
					this->f_event(metadata);
				}
				v_content->v_playing.clear();
			});
			auto delay = a_directive.get("directive.payload.audioItem.stream.progressReport.progressReportDelayInMilliseconds", std::string());
			if (!delay.empty()) v_scheduler.f_run_in(std::chrono::milliseconds(std::stoi(delay)), [this](auto)
			{
				this->f_player_event("ProgressReportDelayElapsed");
			});
			auto interval = a_directive.get("directive.payload.audioItem.stream.progressReport.progressReportIntervalInMilliseconds", std::string());
			if (!interval.empty()) v_scheduler.f_run_every(std::chrono::milliseconds(std::stoi(interval)), [this, token](auto)
			{
				if (v_content->v_playing != token) return false;
				this->f_player_event("ProgressReportIntervalElapsed");
				return true;
			});
		}},
		{std::make_pair("AudioPlayer", "Stop"), [this](const boost::property_tree::ptree&)
		{
			this->f_player_stop();
		}},
		{std::make_pair("AudioPlayer", "ClearQueue"), [this](const boost::property_tree::ptree& a_directive)
		{
			auto behavior = a_directive.get<std::string>("directive.payload.clearBehavior");
			if (behavior == "CLEAR_ENQUEUED") {
				v_content->v_directives.clear();
			} else if (behavior == "CLEAR_ALL") {
				v_content->v_directives.clear();
				this->f_player_stop();
			}
			this->f_empty_event("AudioPlayer", "PlaybackQueueCleared");
		}},
		{std::make_pair("SpeechSynthesizer", "Speak"), [this](const boost::property_tree::ptree& a_directive)
		{
			v_dialog->f_queue([this, token = a_directive.get<std::string>("directive.payload.token"), audio = std::make_shared<t_attached_audio>(*this, v_dialog->v_task, a_directive.get<std::string>("directive.payload.url").substr(4))]
			{
				auto f = [this, token](const std::string& a_name)
				{
					boost::property_tree::ptree metadata;
					this->f_metadata("SpeechSynthesizer", a_name, metadata);
					metadata.put("event.payload.token", token);
					this->f_event(metadata);
				};
				this->f_dialog_acquire(v_dialog->v_task);
				v_dialog->v_target.f_reset();
				v_dialog->v_playing = token;
				f("SpeechStarted");
				try {
					std::unique_ptr<t_audio_source> source( audio->f_open([] {}, [] {}));
					t_audio_decoder decoder(*source);
					v_dialog->f_loop(decoder);
					v_dialog->f_flush();
				} catch (std::exception&) {
				}
				f("SpeechFinished");
				v_dialog->v_playing.clear();
				this->f_dialog_release();
			});
		}},
		{std::make_pair("System", "ResetUserInactivity"), [this](const boost::property_tree::ptree&)
		{
			v_last_activity = std::chrono::steady_clock::now();
		}}
	};
	std::map<std::string, t_attached_audio*> v_id2audio;
	t_channel* v_dialog = nullptr;
	bool v_dialog_busy = false;
	t_channel* v_content = nullptr;
	bool v_pausing = false;
	std::chrono::steady_clock::time_point v_stuttering;
	t_task* v_recognizer = nullptr;
	size_t v_capture_threshold = 32 * 1024;
	size_t v_capture_integral = 0;
	std::chrono::steady_clock::time_point v_capture_exceeded;
	bool v_capture_busy = false;
	bool v_capture_auto = true;
	bool v_capture_fullduplex = false;
	bool v_capture_force = false;
	std::function<void()> v_expecting_speech;
	boost::asio::steady_timer* v_expecting_timeout = nullptr;
	std::string v_expecting_dialog_id;
	std::chrono::steady_clock::time_point v_last_activity;
	std::map<std::string, t_alert> v_alerts;

	void f_reconnect()
	{
		std::fprintf(stderr, "reconnecting...\n");
		v_ready = false;
		if (v_session) {
			v_session->shutdown();
			v_session.reset();
		}
		f_connect();
	}
	void f_context(boost::property_tree::ptree& a_metadata)
	{
		boost::property_tree::ptree player;
		player.put("header.namespace", "AudioPlayer");
		player.put("header.name", "PlaybackState");
		if (v_content->v_playing.empty()) {
			player.put("payload.token", std::string());
			player.put("payload.offsetInMilliseconds", 0);
			player.put("payload.playerActivity", v_content->f_offset() > 0 ? "FINISHED" : "IDLE");
		} else {
			player.put("payload.token", v_content->v_playing);
			player.put("payload.offsetInMilliseconds", v_content->f_offset());
			player.put("payload.playerActivity", v_pausing ? "STOPPED" : v_stuttering > std::chrono::steady_clock::time_point() ? "BUFFER_UNDERRUN" : "PLAYING");
		}
		boost::property_tree::ptree alerts;
		if (!v_alerts.empty()) {
			alerts.put("header.namespace", "Alerts");
			alerts.put("header.name", "AlertsState");
			auto& all = alerts.put("payload.allAlerts", std::string());
			auto& active = alerts.put("payload.activeAlerts", std::string());
			for (auto& x : v_alerts) {
				boost::property_tree::ptree alert;
				alert.put("token", x.first);
				alert.put("type", x.second.v_type);
				alert.put("scheduledTime", x.second.v_at);
				all.push_back(boost::property_tree::ptree::value_type(std::string(), alert));
				if (x.second.v_active) active.push_back(boost::property_tree::ptree::value_type(std::string(), alert));
			}
		}
		boost::property_tree::ptree synthesizer;
		synthesizer.put("header.namespace", "SpeechSynthesizer");
		synthesizer.put("header.name", "SpeechState");
		if (v_dialog->v_playing.empty()) {
			synthesizer.put("payload.token", std::string());
			synthesizer.put("payload.offsetInMilliseconds", 0);
			synthesizer.put("payload.playerActivity", "FINISHED");
		} else {
			synthesizer.put("payload.token", v_dialog->v_playing);
			synthesizer.put("payload.offsetInMilliseconds", v_dialog->f_offset());
			synthesizer.put("payload.playerActivity", "PLAYING");
		}
		auto& context = a_metadata.put("context", std::string());
		context.push_back(boost::property_tree::ptree::value_type(std::string(), player));
		if (!v_alerts.empty()) context.push_back(boost::property_tree::ptree::value_type(std::string(), alerts));
		context.push_back(boost::property_tree::ptree::value_type(std::string(), synthesizer));
	}
	void f_metadata(const std::string& a_namespace, const std::string& a_name, boost::property_tree::ptree& a_metadata)
	{
		std::fprintf(stderr, "event: %s.%s\n", a_namespace.c_str(), a_name.c_str());
		a_metadata.put("event.header.namespace", a_namespace);
		a_metadata.put("event.header.name", a_name);
		a_metadata.put("event.header.messageId", "messateId-" + std::to_string(++v_message_id));
	}
	void f_setup(const nghttp2::asio_http2::client::response& a_response)
	{
		std::fprintf(stderr, "on response(%p): %d\n", &a_response, a_response.status_code());
#ifdef ALEXAAGENT_LOG
		for (auto& x : a_response.header()) std::fprintf(stderr, "%s: %s\n", x.first.c_str(), x.second.value.c_str());
#endif
		auto i = a_response.header().find("content-type");
		if (i == a_response.header().end()) return;
		std::smatch match;
		if (!std::regex_match(i->second.value, match, v_re_content_type)) return;
		a_response.on_data([&a_response, parser = std::make_shared<t_parser>(*this, match[1].str())](auto a_p, size_t a_n)
		{
#ifdef ALEXAAGENT_LOG
			std::fprintf(stderr, "response(%p) on data: %d\n", &a_response, a_n);
#endif
			(*parser)(a_p, a_n);
		});
	}
	template<typename T_data>
	const nghttp2::asio_http2::client::request* f_post(T_data a_data)
	{
		if (!v_ready) {
			std::fprintf(stderr, "not ready.\n");
			return nullptr;
		}
		boost::system::error_code ec;
		auto request = v_session->submit(ec, "POST", "https://avs-alexa-na.amazon.com/v20160207/events", a_data, v_header);
		if (!request) {
			std::fprintf(stderr, "events POST failed: %s\n", ec.message());
			f_reconnect();
			return nullptr;
		}
		std::fprintf(stderr, "events POST(%p) opened.\n", request);
		request->on_response([this, request](auto& a_response)
		{
			std::fprintf(stderr, "events POST(%p) ", request);
			this->f_setup(a_response);
		});
		return request;
	}
	void f_event(const boost::property_tree::ptree& a_metadata)
	{
		auto body = v_boundary_metadata + f_to_json(a_metadata, false) + v_boundary_terminator;
#ifdef ALEXAAGENT_LOG
		std::fprintf(stderr, "request body:\n%s\n", body.c_str());
#endif
		auto request = f_post(body);
		if (request) request->on_close([request](auto a_code)
		{
			std::fprintf(stderr, "events POST(%p) on close: %d\n", request, a_code);
		});
	}
	void f_empty_event(const std::string& a_namespace, const std::string& a_name)
	{
		boost::property_tree::ptree metadata;
		f_metadata(a_namespace, a_name, metadata);
		metadata.put("event.payload.foo", "bar");
		f_event(metadata);
	}
	void f_alerts_event(const std::string& a_name, const std::string& a_token)
	{
		boost::property_tree::ptree metadata;
		f_metadata("Alerts", a_name, metadata);
		metadata.put("event.payload.token", a_token);
		f_event(metadata);
	}
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
		auto i = v_alerts.insert(std::make_pair(a_token, t_alert{a_type, a_at})).first;
		i->second.v_timer.reset(new boost::asio::system_timer(v_scheduler.f_io(), std::chrono::system_clock::from_time_t(t)));
		i->second.v_timer->async_wait([this, i](auto a_ec)
		{
			std::fprintf(stderr, "alert: %s\n", i->first.c_str());
			i->second.v_active = true;
			this->f_alerts_event("AlertStarted", i->first);
			i->second.v_active = false;
			this->f_alerts_event("AlertStopped", i->first);
			v_alerts.erase(i);
			this->f_alerts_save();
		});
	}
	void f_alerts_load()
	{
		std::ifstream s("alerts.json");
		if (!s) return;
		try {
			boost::property_tree::ptree alerts;
			boost::property_tree::read_json(s, alerts);
			for (auto& x : alerts) f_alerts_set(x.second.get<std::string>("token"), x.second.get<std::string>("type"), x.second.get<std::string>("scheduledTime"));
		} catch (std::exception&) {
		}
	}
	void f_alerts_save()
	{
		boost::property_tree::ptree alerts;
		for (auto& x : v_alerts) {
			boost::property_tree::ptree alert;
			alert.put("token", x.first);
			alert.put("type", x.second.v_type);
			alert.put("scheduledTime", x.second.v_at);
			alerts.push_back(boost::property_tree::ptree::value_type(std::string(), alert));
		}
		std::ofstream s("alerts.json");
		boost::property_tree::write_json(s, alerts);
	}
	void f_player_event(const std::string& a_name)
	{
		boost::property_tree::ptree metadata;
		f_metadata("AudioPlayer", a_name, metadata);
		metadata.put("event.payload.token", v_content->v_playing);
		metadata.put("event.payload.offsetInMilliseconds", v_content->f_offset());
		f_event(metadata);
	}
	void f_player_stop()
	{
		if (!v_content->v_playing.empty()) v_content->v_task.f_post([](auto)
		{
			throw nullptr;
		});
	}
	void f_dialog_acquire(t_task& a_task)
	{
		while (v_dialog_busy) a_task.f_wait();
		v_dialog_busy = true;
		if (v_capture_fullduplex) return;
		v_content->v_task.f_post([this, &a_task](auto)
		{
			if (!v_content->v_playing.empty()) this->f_player_event("PlaybackPaused");
			v_pausing = true;
			a_task.f_notify();
			do v_content->v_task.f_wait(); while (v_pausing);
			if (!v_content->v_playing.empty()) this->f_player_event("PlaybackResumed");
		});
		do a_task.f_wait(); while (!v_pausing);
	}
	void f_dialog_release()
	{
		v_dialog_busy = v_pausing = false;
		v_dialog->v_task.f_notify();
		v_content->v_task.f_notify();
		v_recognizer->f_notify();
	}
	bool f_capture(ALCdevice* a_device, char* a_buffer, bool a_busy)
	{
		while (true) {
			if (!a_busy && !v_expecting_timeout && v_expecting_speech) v_expecting_speech();
			if ((v_capture_busy && v_capture_auto && v_dialog->v_playing.empty() && (v_capture_fullduplex || v_content->v_playing.empty()) || v_capture_force) != a_busy) return false;
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
		size_t m = v_capture_threshold / 1024;
		size_t n = v_capture_integral / 1024;
		std::fprintf(stderr, "%s: %s\x1b[K\r", v_capture_busy ? "BUSY" : "IDLE", (n > m ? std::string(m, '#') + std::string(std::min(n, size_t(72)) - m, '=') : std::string(n, '#')).c_str());
		return true;
	}
	void f_recognizer()
	{
		while (true) {
			std::unique_ptr<ALCdevice, decltype(&alcCaptureCloseDevice)> device(alcCaptureOpenDevice(NULL, 16000, AL_FORMAT_MONO16, 1600), alcCaptureCloseDevice);
			if (!device) {
				std::fprintf(stderr, "alcCaptureOpenDevice: %d\n", alGetError());
				v_recognizer->f_wait(std::chrono::seconds(5));
				continue;
			}
			alcCaptureStart(device.get());
			auto queue = std::make_shared<std::pair<std::deque<char>, bool>>(std::make_pair(std::deque<char>(), false));
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
				f_dialog_acquire(*v_recognizer);
			}
			std::fprintf(stderr, "recognize started.\n");
			deque.insert(deque.begin(), v_boundary_audio.begin(), v_boundary_audio.end());
			{
				boost::property_tree::ptree metadata;
				f_context(metadata);
				f_metadata("SpeechRecognizer", "Recognize", metadata);
				if (v_expecting_dialog_id.empty()) {
					metadata.put("event.header.dialogRequestId", "dialogRequestId-" + std::to_string(++v_dialog_id));
				} else {
					metadata.put("event.header.dialogRequestId", v_expecting_dialog_id);
					v_expecting_dialog_id.clear();
				}
				metadata.put("event.payload.profile", "CLOSE_TALK");
				metadata.put("event.payload.format", "AUDIO_L16_RATE_16000_CHANNELS_1");
				auto s = f_to_json(metadata, false);
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
					std::fprintf(stderr, "events POST(%p) on close: %d\n", *p, a_code);
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
				std::fprintf(stderr, "recognize finished.\n");
			} else {
				std::fprintf(stderr, "recognize failed.\n");
				while (f_capture(device.get(), buffer, true));
				f_dialog_release();
			}
			v_last_activity = std::chrono::steady_clock::now();
		}
	}
	void f_connect()
	{
		v_session.reset(new nghttp2::asio_http2::client::session(v_scheduler.f_io(), v_tls, "avs-alexa-na.amazon.com", "https"));
		v_session->read_timeout(boost::posix_time::hours(1));
		std::fprintf(stderr, "session(%p) created\n", v_session.get());
		v_session->on_connect([this](auto)
		{
			boost::system::error_code ec;
			auto request = v_session->submit(ec, "GET", "https://avs-alexa-na.amazon.com/v20160207/directives", v_header);
			if (!request) {
				std::fprintf(stderr, "directives GET failed: %s\n", ec.message());
				this->f_reconnect();
				return;
			}
			std::fprintf(stderr, "directives GET(%p) opened.\n", request);
			request->on_response([this, request](auto& a_response)
			{
				std::fprintf(stderr, "directives GET(%p) ", request);
				this->f_setup(a_response);
				v_ready = true;
				boost::property_tree::ptree metadata;
				this->f_context(metadata);
				this->f_metadata("System", "SynchronizeState", metadata);
				metadata.put("event.payload.foo", "bar");
				this->f_event(metadata);
				this->f_alerts_load();
			});
			request->on_close([request](auto a_code)
			{
				std::fprintf(stderr, "directives GET(%p) on close: %d\n", request, a_code);
			});
		});
		v_session->on_error([this](auto a_ec)
		{
			std::fprintf(stderr, "session(%p) on error: %s\n", v_session.get(), a_ec.message().c_str());
			this->f_reconnect();
		});
	}

public:
	t_session(t_scheduler& a_scheduler, const std::string& a_token) : v_tls(boost::asio::ssl::context::tlsv12), v_scheduler(a_scheduler)
	{
		v_tls.set_default_verify_paths();
		boost::system::error_code ec;
		nghttp2::asio_http2::client::configure_tls_context(ec, v_tls);
		f_token(a_token);
		v_scheduler.f_spawn([this](auto& a_task)
		{
			t_channel dialog(a_task);
			v_dialog = &dialog;
			dialog.f_run();
		});
		v_scheduler.f_spawn([this](auto& a_task)
		{
			t_channel content(a_task);
			v_content = &content;
			content.f_run();
		});
		v_scheduler.f_spawn([this](auto& a_task)
		{
			v_recognizer = &a_task;
			this->f_recognizer();
		});
		v_scheduler.f_run_every(std::chrono::hours(1), [this](auto)
		{
			boost::property_tree::ptree metadata;
			this->f_metadata("System", "UserInactivityReport", metadata);
			metadata.put("event.payload.inactiveTimeInSeconds", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - v_last_activity).count());
			this->f_event(metadata);
			return true;
		});
		f_connect();
	}
	void f_token(const std::string& a_token)
	{
		v_header = nghttp2::asio_http2::header_map{
			{"authorization", {"Bearer " + a_token, true}},
			{"content-type", {"multipart/form-data; boundary=this-is-a-boundary", true}}
		};
	}
	void f_recognize(bool a_run)
	{
		v_capture_force = a_run;
		v_recognizer->f_notify();
	}
};

std::string t_session::v_boundary_metadata =
	"--this-is-a-boundary\r\n"
	"Content-Disposition: form-data; name=\"metadata\"\r\n"
	"Content-Type: application/json; charset=UTF-8\r\n\r\n";
std::string t_session::v_boundary_audio =
	"\r\n--this-is-a-boundary\r\n"
	"Content-Disposition: form-data; name=\"audio\"\r\n"
	"Content-Type: application/octet-stream\r\n\r\n";
std::string t_session::v_boundary_terminator = "\r\n--this-is-a-boundary--\r\n";
std::regex t_session::v_re_content_type("\\s*multipart/related\\s*;\\s*boundary\\s*=\\s*([\\-0-9A-Za-z]+).*");

int main(int argc, char* argv[])
{
	boost::property_tree::ptree profile;
	{
		std::ifstream s("profile.json");
		boost::property_tree::read_json(s, profile);
	}
	av_register_all();
	avformat_network_init();
	std::unique_ptr<ALCdevice, decltype(&alcCloseDevice)> device(alcOpenDevice(NULL), alcCloseDevice);
	if (!device) throw std::runtime_error("alcOpenDevice");
	std::unique_ptr<ALCcontext, void (*)(ALCcontext*)> context(alcCreateContext(device.get(), NULL), [](auto a_x)
	{
		alcMakeContextCurrent(NULL);
		alcDestroyContext(a_x);
	});
	alcMakeContextCurrent(context.get());
	alGetError();
	nghttp2::asio_http2::server::http2 server;
	std::unique_ptr<t_scheduler> scheduler;
	std::unique_ptr<t_session> session;
	std::function<void(const std::string&)> f_refresh;
	auto grant = [&](std::map<std::string, std::string>&& a_query, std::function<void()> a_next)
	{
		a_query.emplace("client_id", profile.get<std::string>("client_id"));
		a_query.emplace("client_secret", profile.get<std::string>("client_secret"));
		f_https_post(*server.io_services().front(), "api.amazon.com", "/auth/o2/token", a_query, [&, a_next](auto a_code, auto&, auto& a_content)
		{
			boost::property_tree::ptree result;
			boost::property_tree::read_json(a_content, result);
#ifdef ALEXAAGENT_LOG
			std::fprintf(stderr, "grant: %d, %s\n", a_code, f_to_json(result, true).c_str());
#endif
			if (a_code == 200) {
				auto access_token = result.get<std::string>("access_token");
				auto refresh_token = result.get<std::string>("refresh_token");
				std::ofstream("token") << refresh_token;
				if (session)
					session->f_token(access_token);
				else
					session.reset(new t_session(*scheduler, access_token));
				scheduler->f_run_in(std::chrono::seconds(result.get<size_t>("expires_in")), [&, refresh_token](auto)
				{
					f_refresh(refresh_token);
				});
			}
			a_next();
		}, [a_next](auto a_ec)
		{
			std::fprintf(stderr, "grant: %s\n", a_ec.message().c_str());
			a_next();
		});
	};
	f_refresh = [&](const std::string& a_token)
	{
		grant({
			{"grant_type", "refresh_token"},
			{"refresh_token", a_token}
		}, [] {});
	};
	server.handle("/", [&](auto& a_request, auto& a_response)
	{
		if (std::ifstream("token")) {
			a_response.write_head(200);
			a_response.end(nghttp2::asio_http2::file_generator("index.html"));
		} else {
			boost::property_tree::ptree data;
			data.put("alexa:all.productID", profile.get<std::string>("product_id"));
			data.put("alexa:all.productInstanceAttributes.deviceSerialNumber", profile.get<size_t>("device_serial_number"));
			nghttp2::asio_http2::server::redirect_handler(302, "https://www.amazon.com/ap/oa?" + f_build_query_string({
				{"client_id", profile.get<std::string>("client_id")},
				{"scope", "alexa:all"},
				{"scope_data", f_to_json(data, false)},
				{"response_type", "code"},
				{"redirect_uri", profile.get<std::string>("redirect_uri")}
			}))(a_request, a_response);
		}
	});
	server.handle("/grant", [&](auto& a_request, auto& a_response)
	{
		auto f = std::bind(nghttp2::asio_http2::server::redirect_handler(302, "/"), std::ref(a_request), std::ref(a_response));
		auto values = f_parse_query_string(a_request.uri().raw_query);
		auto i = values.find("code");
		if (i == values.end())
			f();
		else
			grant({
				{"grant_type", "authorization_code"},
				{"code", i->second},
				{"redirect_uri", profile.get<std::string>("redirect_uri")}
			}, f);
	});
	server.handle("/recognize/start", [&](auto& a_request, auto& a_response)
	{
		if (session) session->f_recognize(true);
		a_response.write_head(session ? 200 : 500);
		a_response.end();
	});
	server.handle("/recognize/finish", [&](auto& a_request, auto& a_response)
	{
		if (session) session->f_recognize(false);
		a_response.write_head(session ? 200 : 500);
		a_response.end();
	});
	boost::system::error_code ec;
	boost::asio::ssl::context tls(boost::asio::ssl::context::tlsv12);
	tls.use_private_key_file("server.key", boost::asio::ssl::context::pem);
	tls.use_certificate_chain_file("server.crt");
	nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls);
	if (server.listen_and_serve(ec, tls, "localhost", "3000", true)) throw std::runtime_error(ec.message());
	scheduler.reset(new t_scheduler(*server.io_services().front()));
	boost::asio::signal_set signals(*server.io_services().front(), SIGINT);
	signals.async_wait([&](auto, auto a_signal)
	{
		std::fprintf(stderr, "\ncaught signal: %d\n", a_signal);
		scheduler->f_shutdown(std::bind(&nghttp2::asio_http2::server::http2::stop, std::ref(server)));
	});
	{
		std::string token;
		std::ifstream("token") >> token;
		if (!token.empty()) f_refresh(token);
	}
	server.join();
	std::fprintf(stderr, "server stopped.\n");
	return 0;
}
