#include <nghttp2/asio_http2_server.h>
#include <Simple-WebSocket-Server/server_wss.hpp>

#include "session.h"
#include "tiny_http.h"

template<typename T_server>
void f_web_socket(t_session* a_session, std::shared_ptr<T_server> a_server)
{
	std::map<std::string, std::function<void(const picojson::value&)>> handlers{
		{"hello", [&](auto a_x)
		{
			a_session->v_state_changed();
			a_session->v_options_changed();
		}},
		{"capture.threshold", [&](auto a_x)
		{
			a_session->f_capture_threshold(a_x.template get<double>());
		}},
		{"capture.auto", [&](auto a_x)
		{
			a_session->f_capture_auto(a_x.template get<bool>());
		}},
		{"capture.force", [&](auto a_x)
		{
			a_session->f_capture_force(a_x.template get<bool>());
		}},
		{"alerts.duration", [&](auto a_x)
		{
			a_session->f_alerts_duration(std::min(static_cast<size_t>(a_x.template get<double>()), size_t(3600)));
		}},
		{"alerts.stop", [&](auto a_x)
		{
			a_session->f_alerts_stop(a_x.template get<std::string>());
		}},
		{"content.background", [&](auto a_x)
		{
			a_session->f_content_can_play_in_background(a_x.template get<bool>());
		}},
		{"speaker.volume", [&](auto a_x)
		{
			a_session->f_speaker_volume(a_x.template get<double>());
		}},
		{"speaker.muted", [&](auto a_x)
		{
			a_session->f_speaker_muted(a_x.template get<bool>());
		}},
		{"playback.play", [&](auto)
		{
			a_session->f_playback_play();
		}},
		{"playback.pause", [&](auto)
		{
			a_session->f_playback_pause();
		}},
		{"playback.next", [&](auto)
		{
			a_session->f_playback_next();
		}},
		{"playback.previous", [&](auto)
		{
			a_session->f_playback_previous();
		}}
	};
	auto& ws = a_server->endpoint["^/session$"];
	ws.onmessage = [&](auto a_connection, auto a_message)
	{
		auto s = a_message->string();
		std::fprintf(stderr, "on message: %s\n", s.c_str());
		picojson::value message;
		picojson::parse(message, s.begin(), s.end(), nullptr);
		a_session->f_scheduler().f_strand().dispatch([&, message = std::move(message)]
		{
			try {
				for (auto& x : message.get<picojson::value::object>()) handlers.at(x.first)(x.second);
			} catch (std::exception& e) {
				std::fprintf(stderr, "failed: %s\n", e.what());
			}
		});
	};
	auto send = [&](const std::string& a_name, picojson::value::object&& a_values)
	{
		auto message = picojson::value(picojson::value::object{
			{a_name, picojson::value(std::move(a_values))}
		}).serialize();
		for (auto& x : a_server->get_connections()) {
			auto s = std::make_shared<typename T_server::SendStream>();
			*s << message;
			a_server->send(x, s);
		}
	};
	a_session->v_capture = [&]
	{
		send("capture", {
			{"integral", picojson::value(static_cast<double>(a_session->f_capture_integral()))},
			{"busy", picojson::value(a_session->f_capture_busy())}
		});
	};
	a_session->v_state_changed = [&]
	{
		picojson::value::array alerts;
		for (auto& x : a_session->f_alerts()) alerts.push_back(picojson::value(picojson::value::object{
			{"token", picojson::value(x.first)},
			{"type", picojson::value(x.second.f_type())},
			{"at", picojson::value(x.second.f_at())},
			{"active", picojson::value(x.second.f_active())}
		}));
		send("state_changed", {
			{"dialog", picojson::value(picojson::value::object{
				{"active", picojson::value(a_session->f_dialog_active())},
				{"playing", picojson::value(a_session->f_dialog_playing())},
				{"expecting_speech", picojson::value(a_session->f_expecting_speech())}
			})},
			{"alerts", picojson::value(std::move(alerts))},
			{"content", picojson::value(picojson::value::object{
				{"playing", picojson::value(a_session->f_content_playing())}
			})}
		});
	};
	a_session->v_options_changed = [&]
	{
		send("options_changed", {
			{"alerts", picojson::value(picojson::value::object{
				{"duration", picojson::value(static_cast<double>(a_session->f_alerts_duration()))}
			})},
			{"content", picojson::value(picojson::value::object{
				{"can_play_in_background", picojson::value(a_session->f_content_can_play_in_background())}
			})},
			{"speaker", picojson::value(picojson::value::object{
				{"volume", picojson::value(static_cast<double>(a_session->f_speaker_volume()))},
				{"muted", picojson::value(a_session->f_speaker_muted())}
			})},
			{"capture", picojson::value(picojson::value::object{
				{"threshold", picojson::value(static_cast<double>(a_session->f_capture_threshold()))},
				{"auto", picojson::value(a_session->f_capture_auto())},
				{"force", picojson::value(a_session->f_capture_force())}
			})}
		});
	};
	a_server->start();
}

void f_override_open_audio_by_url(t_session& a_session)
{
	a_session.v_open_audio_by_url = [open = a_session.v_open_audio_by_url](auto& a_url)
	{
		try {
			auto url = a_url;
			while (true) {
				std::fprintf(stderr, "opening: %s\n", url.c_str());
				try {
					return open(url);
				} catch (std::exception& e) {
					std::fprintf(stderr, "caught: %s\ntrying to resolve...\n", e.what());
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
							std::fprintf(stderr, "found x-mpegurl.\n");
							boost::asio::read_until(a_socket, http.v_buffer, '\n', ec);
							if (ec && ec != boost::asio::error::eof) throw ec;
							std::string line;
							std::getline(std::istream(&http.v_buffer), line);
							if (!std::regex_match(line, match, std::regex{"\\s*(https?://\\S+)\\s*\r?"})) throw std::runtime_error("invalid url");
							url = match[1];
						} else if (match[1] == "x-scpls") {
							std::fprintf(stderr, "found x-scpls.\n");
							boost::asio::read(a_socket, http.v_buffer, ec);
							if (ec && ec != boost::asio::error::eof) throw ec;
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
			std::fprintf(stderr, "caught: %s\n", e.what());
			throw nullptr;
		}
	};
}

int main(int argc, char* argv[])
{
	picojson::value configuration;
	{
		std::ifstream s("configuration/session.json");
		s >> configuration;
	}
	auto profile = configuration / "profile";
	auto service = configuration / "service";
	auto service_host = service / "host"_jss;
	auto http2port = std::to_string(static_cast<int>(service / "http2"_jsn));
	int wsport = service / "websocket"_jsn;
	auto service_key = service * "key" | std::string();
	auto sounds = configuration / "sounds";
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
	std::thread wsthread;
	std::function<void()> wsstop;
	auto start = [&](auto a_server)
	{
		f_override_open_audio_by_url(*session);
		wsthread = std::thread(std::bind(f_web_socket<typename decltype(a_server)::element_type>, session.get(), a_server));
		wsstop = [a_server]
		{
			a_server->stop();
		};
	};
	std::function<void(const std::string&)> refresh;
	auto grant = [&](std::map<std::string, std::string>&& a_query, std::function<void()> a_done)
	{
		a_query.emplace("client_id", profile / "client_id"_jss);
		a_query.emplace("client_secret", profile / "client_secret"_jss);
		auto http = std::make_shared<t_http10>("https://api.amazon.com/auth/o2/token");
		(*http)("POST", a_query)(*server.io_services().front(), [&, a_done, http](auto a_socket)
		{
			boost::asio::async_read(*a_socket, http->v_buffer, [&, a_done, http, a_socket](auto a_ec, auto)
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
					if (session) {
						session->f_token(access_token);
					} else {
						session.reset(new t_session(*scheduler, access_token, sounds));
						if (service_key.empty())
							start(std::make_shared<SimpleWeb::SocketServer<SimpleWeb::WS>>(wsport));
						else
							start(std::make_shared<SimpleWeb::SocketServer<SimpleWeb::WSS>>(wsport, 1, service / "certificate"_jss, service_key));
					}
					scheduler->f_run_in(std::chrono::seconds(expires_in), [&, refresh_token](auto)
					{
						refresh(refresh_token);
					});
				}
				a_done();
			});
		}, [a_done](auto a_ec)
		{
			std::fprintf(stderr, "grant: %s\n", a_ec.message().c_str());
			a_done();
		});
	};
	refresh = [&](const std::string& a_token)
	{
		grant({
			{"grant_type", "refresh_token"},
			{"refresh_token", a_token}
		}, [] {});
	};
	server.handle("/", [&](auto& a_request, auto& a_response)
	{
		if (std::ifstream("session/token")) {
			a_response.write_head(200);
			a_response.end(nghttp2::asio_http2::file_generator("index.html"));
		} else {
			picojson::value data(picojson::value::object{
				{"alexa:all", picojson::value(picojson::value::object{
					{"productID", profile / "product_id"},
					{"productInstanceAttributes", picojson::value(picojson::value::object{
						{"deviceSerialNumber", profile / "device_serial_number"}
					})}
				})},
			});
			nghttp2::asio_http2::server::redirect_handler(302, "https://www.amazon.com/ap/oa?" + f_build_query_string({
				{"client_id", profile / "client_id"_jss},
				{"scope", "alexa:all"},
				{"scope_data", data.serialize()},
				{"response_type", "code"},
				{"redirect_uri", profile / "redirect_uri"_jss}
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
				{"redirect_uri", profile / "redirect_uri"_jss}
			}, f);
	});
	server.handle("/session", [&](auto&, auto& a_response)
	{
		a_response.write_head(200);
		a_response.end((service_key.empty() ? "ws://" : "wss://") + service_host + ':' + std::to_string(wsport) + "/session");
	});
	boost::asio::ssl::context tls(boost::asio::ssl::context::tlsv12);
	boost::system::error_code ec;
	if (service_key.empty()) {
		if (server.listen_and_serve(ec, service_host, http2port, true)) throw std::runtime_error(ec.message());
	} else {
		tls.use_private_key_file(service_key, boost::asio::ssl::context::pem);
		tls.use_certificate_chain_file(service / "certificate"_jss);
		nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls);
		if (server.listen_and_serve(ec, tls, service_host, http2port, true)) throw std::runtime_error(ec.message());
	}
	scheduler.reset(new t_scheduler(*server.io_services().front()));
	boost::asio::signal_set signals(*server.io_services().front(), SIGINT);
	signals.async_wait([&](auto, auto a_signal)
	{
		std::fprintf(stderr, "\ncaught signal: %d\n", a_signal);
		scheduler->f_shutdown(std::bind(&nghttp2::asio_http2::server::http2::stop, std::ref(server)));
		if (wsstop) wsstop();
	});
	{
		std::string token;
		std::ifstream("session/token") >> token;
		if (!token.empty()) refresh(token);
	}
	server.join();
	if (wsthread.joinable()) wsthread.join();
	std::fprintf(stderr, "server stopped.\n");
	return 0;
}
