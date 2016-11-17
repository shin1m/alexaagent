#ifndef ALEXAAGENT__AGENT_H
#define ALEXAAGENT__AGENT_H

#include <fstream>

#include "session.h"
#include "tiny_http.h"

class t_agent : public t_session
{
	std::string v_id;
	std::string v_secret;
	ALuint v_sounds[4];
	size_t v_refresh_retry_interval = 1;

	void f_load_sound(ALuint a_buffer, const std::string& a_path)
	{
		t_url_audio_source source(a_path.c_str());
		t_audio_decoder decoder(source);
		ALenum format = AL_FORMAT_MONO8;
		std::vector<char> data;
		ALsizei rate = 0;
		decoder([&](size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
		{
			format = a_channels == 1 ? (a_bytes == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16) : (a_bytes == 1 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16);
			data.insert(data.end(), a_p, a_p + a_n);
			rate = a_rate;
		});
		alBufferData(a_buffer, format, data.data(), data.size(), rate);
	}

public:
	std::function<void()> v_capture;
	std::function<void()> v_options_changed;

	t_agent(t_scheduler& a_scheduler, const std::function<std::ostream&(t_severity)>& a_log, const std::string& a_id, const std::string& a_secret, const picojson::value& a_sounds) : t_session(a_scheduler, a_log, [this](auto a_type)
	{
		ALuint x;
		alGenSources(1, &x);
		std::shared_ptr<ALuint> source{new ALuint(x), [](auto a_x)
		{
			alDeleteSources(1, a_x);
		}};
		alSourcei(*source, AL_LOOPING, AL_TRUE);
		return [source, sounds = v_sounds + (a_type == "TIMER" ? 0 : 2)](bool a_background)
		{
			alSourceStop(*source);
			alSourcei(*source, AL_BUFFER, sounds[a_background ? 1 : 0]);
			alSourcef(*source, AL_GAIN, a_background ? 1.0f / 16.0f : 1.0f);
			alSourcePlay(*source);
		};
	}), v_id(a_id), v_secret(a_secret)
	{
		alGenBuffers(4, v_sounds);
		f_load_sound(v_sounds[0], a_sounds / "timer" / "foreground"_jss);
		f_load_sound(v_sounds[1], a_sounds / "timer" / "background"_jss);
		f_load_sound(v_sounds[2], a_sounds / "alarm" / "foreground"_jss);
		f_load_sound(v_sounds[3], a_sounds / "alarm" / "background"_jss);
		try {
			picojson::value options;
			std::ifstream s("session/options.json");
			s >> options;
			f_alerts_duration(options / "alerts_duration"_jsn);
			f_content_can_play_in_background(options / "content_can_play_in_background"_jsb);
			f_capture_threshold(options / "capture_threshold"_jsn);
			f_capture_auto(options / "capture_auto"_jsb);
		} catch (std::exception& e) {
			if (v_log) v_log(e_severity__ERROR) << "loading session/options.json: " << e.what() << std::endl;
		}
		try {
			picojson::value alerts;
			std::ifstream s("session/alerts.json");
			s >> alerts;
			for (auto& x : alerts.get<picojson::value::object>()) f_alerts_set(x.first, x.second / "type"_jss, x.second / "scheduledTime"_jss);
		} catch (std::exception& e) {
			if (v_log) v_log(e_severity__ERROR) << "loading session/alerts.json: " << e.what() << std::endl;
		}
		try {
			picojson::value speaker;
			std::ifstream s("session/speaker.json");
			s >> speaker;
			f_speaker_volume(speaker / "volume"_jsn);
			f_speaker_muted(speaker / "muted"_jsb);
		} catch (std::exception& e) {
			if (v_log) v_log(e_severity__ERROR) << "loading session/speaker.json: " << e.what() << std::endl;
		}
		t_session::v_capture = [this]
		{
			size_t m = f_capture_threshold() / 1024;
			size_t n = f_capture_integral() / 1024;
			std::cerr
				<< (f_capture_busy() ? "BUSY" : "IDLE") << ": "
				<< (n > m ? std::string(m, '#') + std::string(std::min(n, size_t(72)) - m, '=') : std::string(n, '#') + std::string(m - n, ' ') + '|')
				<< "\x1b[K\r";
			if (v_capture) v_capture();
		};
		t_session::v_options_changed = [this]
		{
			std::ofstream s("session/options.json");
			picojson::value(picojson::value::object{
				{"alerts_duration", picojson::value(static_cast<double>(f_alerts_duration()))},
				{"content_can_play_in_background", picojson::value(f_content_can_play_in_background())},
				{"capture_threshold", picojson::value(static_cast<double>(f_capture_threshold()))},
				{"capture_auto", picojson::value(f_capture_auto())}
			}).serialize(std::ostreambuf_iterator<char>(s), true);
			if (v_options_changed) v_options_changed();
		};
		v_alerts_changed = [this]
		{
			picojson::value alerts(picojson::value::object{});
			for (auto& x : f_alerts()) alerts << x.first & picojson::value::object{
				{"type", picojson::value(x.second.f_type())},
				{"scheduledTime", picojson::value(x.second.f_at())}
			};
			std::ofstream s("session/alerts.json");
			alerts.serialize(std::ostreambuf_iterator<char>(s), true);
			if (v_state_changed) v_state_changed();
		};
		v_speaker_changed = [this]
		{
			std::ofstream s("session/speaker.json");
			picojson::value(picojson::value::object{
				{"volume", picojson::value(static_cast<double>(f_speaker_volume()))},
				{"muted", picojson::value(f_speaker_muted())}
			}).serialize(std::ostreambuf_iterator<char>(s), true);
			if (v_options_changed) v_options_changed();
		};
		v_open_audio_by_url = [this, open = v_open_audio_by_url](auto& a_url)
		{
			try {
				auto url = a_url;
				while (true) {
					if (v_log) v_log(e_severity__TRACE) << "opening: " << url.c_str() << std::endl;
					try {
						return open(url);
					} catch (std::exception& e) {
						if (v_log) v_log(e_severity__INFORMATION) << "caught: " << e.what() << std::endl << "trying to resolve..." << std::endl;
						boost::asio::io_service io;
						t_http10 http(url);
						t_audio_source* source = nullptr;
						http("GET")(io, [&](auto& a_socket)
						{
							if (http.v_code != 200) throw std::runtime_error("invalid code");
							std::smatch match;
							std::regex content_type{"Content-Type:\\s*\\S+/([^\\s;]+)\\s*;?.*\r"};
							for (auto& x : http.v_headers) if (std::regex_match(x, match, content_type)) break;
							if (match.empty()) throw std::runtime_error("no Content-Type found");
							boost::system::error_code ec;
							if (match[1] == "x-mpegurl") {
								if (v_log) v_log(e_severity__TRACE) << "found x-mpegurl." << std::endl;
								boost::asio::read_until(a_socket, http.v_buffer, '\n', ec);
								if (ec && ec != boost::asio::error::eof) throw boost::system::system_error(ec);
								std::string line;
								std::getline(std::istream(&http.v_buffer), line);
								if (!std::regex_match(line, match, std::regex{"\\s*(https?://\\S+)\\s*\r?"})) throw std::runtime_error("invalid url");
								url = match[1];
							} else if (match[1] == "x-scpls") {
								if (v_log) v_log(e_severity__TRACE) << "found x-scpls." << std::endl;
								boost::asio::read(a_socket, http.v_buffer, ec);
								if (ec && ec != boost::asio::error::eof) throw boost::system::system_error(ec);
								auto buffer = std::make_shared<boost::asio::streambuf>();
								std::ostream stream(buffer.get());
								stream <<
								"#EXTM3U\n"
								"#EXT-X-TARGETDURATION:0\n";
								std::regex file{"File\\d+\\s*=\\s*(https?://\\S+)\\s*\r?"};
								std::string line;
								while (std::getline(std::istream(&http.v_buffer), line))
									if (std::regex_match(line, match, file)) stream << "#EXTINF:0\n" << match[1] << '\n';
								stream << "#EXT-X-ENDLIST\n";
								source = new t_callback_audio_source([buffer](auto a_p, auto a_n)
								{
									return buffer->sgetn(reinterpret_cast<char*>(a_p), a_n);
								});
							} else {
								throw std::runtime_error("unknown Content-Type: " + match[1].str());
							}
						});
						if (source) return source;
					}
				}
			} catch (std::exception& e) {
				if (v_log) v_log(e_severity__ERROR) << "caught: " << e.what() << std::endl;
				throw nullptr;
			}
		};
	}
	~t_agent()
	{
		alDeleteBuffers(4, v_sounds);
	}
	void f_grant(std::map<std::string, std::string>&& a_query, std::function<void(const boost::system::error_code&)> a_done)
	{
		a_query.emplace("client_id", v_id);
		a_query.emplace("client_secret", v_secret);
		auto http = std::make_shared<t_http10>("https://api.amazon.com/auth/o2/token");
		(*http)("POST", a_query)(f_scheduler().f_io(), [this, a_done, http](auto a_socket)
		{
			boost::asio::async_read(*a_socket, http->v_buffer, this->f_scheduler().wrap([this, a_done, http, a_socket](auto a_ec, auto)
			{
				std::istream stream(&http->v_buffer);
				picojson::value result;
				stream >> result;
#ifdef ALEXAAGENT_LOG
				std::fprintf(stderr, "grant: %s %d%s\n%s\n", http->v_http.c_str(), http->v_code, http->v_message.c_str(), result.serialize(true).c_str());
#endif
				if (http->v_code == 200) {
					auto access_token = result / "access_token"_jss;
					size_t expires_in = result / "expires_in"_jsn;
					auto refresh_token = result / "refresh_token"_jss;
					std::ofstream("session/token") << refresh_token;
					this->f_token(access_token);
					this->f_scheduler().f_run_in(std::chrono::seconds(expires_in), [this, refresh_token](auto)
					{
						this->f_refresh(refresh_token);
					});
					a_done({});
				} else {
					a_done(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
				}
			}));
		}, f_scheduler().wrap([this, a_done](auto a_ec)
		{
			std::fprintf(stderr, "grant: %s\n", a_ec.message().c_str());
			a_done(a_ec);
		}));
	}
	void f_refresh(const std::string& a_token)
	{
		f_grant({
			{"grant_type", "refresh_token"},
			{"refresh_token", a_token}
		}, [this, a_token](auto a_ec)
		{
			if (a_ec) {
				if (v_log) v_log(e_severity__ERROR) << "grant retry in " << v_refresh_retry_interval << " seconds." << std::endl;
				this->f_scheduler().f_run_in(std::chrono::seconds(v_refresh_retry_interval), [this, a_token](auto)
				{
					this->f_refresh(a_token);
				});
				if (v_refresh_retry_interval < 256) v_refresh_retry_interval *= 2;
			} else {
				v_refresh_retry_interval = 1;
			}
		});
	}
};

#endif
