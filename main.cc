#include <nghttp2/asio_http2_server.h>
#include <Simple-WebSocket-Server/server_wss.hpp>

#include "session.h"
#include "tiny_http.h"

#define ALEXAAGENT_TLS

template<typename T_server>
void f_web_socket(t_session* a_session, T_server& a_server)
{
	std::map<std::string, std::function<void(const picojson::value&)>> handlers{
		{"hello", [&](auto a_x)
		{
			a_session->v_options_changed();
		}},
		{"threshold", [&](auto a_x)
		{
			a_session->f_capture_threshold(a_x.template get<double>());
		}},
		{"auto", [&](auto a_x)
		{
			a_session->f_capture_auto(a_x.template get<bool>());
		}},
		{"fullduplex", [&](auto a_x)
		{
			a_session->f_capture_fullduplex(a_x.template get<bool>());
		}},
		{"force", [&](auto a_x)
		{
			a_session->f_capture_force(a_x.template get<bool>());
		}},
		{"background", [&](auto a_x)
		{
			a_session->f_content_can_play_in_background(a_x.template get<bool>());
		}},
		{"play", [&](auto)
		{
			a_session->f_playback_play();
		}},
		{"pause", [&](auto)
		{
			a_session->f_playback_pause();
		}},
		{"next", [&](auto)
		{
			a_session->f_playback_next();
		}},
		{"previous", [&](auto)
		{
			a_session->f_playback_previous();
		}}
	};
	auto& ws = a_server.endpoint["^/session$"];
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
		for (auto& x : a_server.get_connections()) {
			auto s = std::make_shared<typename T_server::SendStream>();
			*s << message;
			a_server.send(x, s);
		}
	};
	a_session->v_state_changed = [&]
	{
		send("state_changed", {
			{"dialog", picojson::value(picojson::value::object{
				{"active", picojson::value(a_session->f_dialog_active())},
				{"expecting_speech", picojson::value(a_session->f_expecting_speech())}
			})},
			{"content", picojson::value(picojson::value::object{
				{"playing", picojson::value(a_session->f_content_playing())}
			})},
			{"speaker", picojson::value(picojson::value::object{
				{"volume", picojson::value(static_cast<double>(a_session->f_speaker_volume()))},
				{"muted", picojson::value(a_session->f_speaker_muted())}
			})},
			{"capture", picojson::value(picojson::value::object{
				{"integral", picojson::value(static_cast<double>(a_session->f_capture_integral()))},
				{"busy", picojson::value(a_session->f_capture_busy())}
			})}
		});
	};
	a_session->v_options_changed = [&]
	{
		send("options_changed", {
			{"content", picojson::value(picojson::value::object{
				{"can_play_in_background", picojson::value(a_session->f_content_can_play_in_background())}
			})},
			{"capture", picojson::value(picojson::value::object{
				{"threshold", picojson::value(static_cast<double>(a_session->f_capture_threshold()))},
				{"auto", picojson::value(a_session->f_capture_auto())},
				{"fullduplex", picojson::value(a_session->f_capture_fullduplex())},
				{"force", picojson::value(a_session->f_capture_force())}
			})}
		});
	};
	a_server.start();
}

int main(int argc, char* argv[])
{
	picojson::value configuration;
	{
		std::ifstream s("configuration/session.json");
		s >> configuration;
	}
	auto profile = configuration / "profile";
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
#ifdef ALEXAAGENT_TLS
	SimpleWeb::SocketServer<SimpleWeb::WSS> wsserver(3002, 1, "configuration/server.crt", "configuration/server.key");
#else
	SimpleWeb::SocketServer<SimpleWeb::WS> wsserver(3002);
#endif
	std::thread wsthread;
	std::function<void(const std::string&)> f_refresh;
	auto grant = [&](std::map<std::string, std::string>&& a_query, std::function<void()> a_done)
	{
		a_query.emplace("client_id", profile / "client_id"_jss);
		a_query.emplace("client_secret", profile / "client_secret"_jss);
		f_https_post(*server.io_services().front(), "api.amazon.com", "/auth/o2/token", a_query, [&, a_done](auto a_code, auto&, auto& a_content)
		{
			picojson::value result;
			a_content >> result;
#ifdef ALEXAAGENT_LOG
			std::fprintf(stderr, "grant: %d, %s\n", a_code, result.serialize(true).c_str());
#endif
			if (a_code == 200) {
				auto access_token = result / "access_token"_jss;
				size_t expires_in = result / "expires_in"_jsn;
				auto refresh_token = result / "refresh_token"_jss;
				std::ofstream("session/token") << refresh_token;
				if (session) {
					session->f_token(access_token);
				} else {
					session.reset(new t_session(*scheduler, access_token, sounds));
					wsthread = std::thread(std::bind(f_web_socket<decltype(wsserver)>, session.get(), std::ref(wsserver)));
				}
				scheduler->f_run_in(std::chrono::seconds(expires_in), [&, refresh_token](auto)
				{
					f_refresh(refresh_token);
				});
			}
			a_done();
		}, [a_done](auto a_ec)
		{
			std::fprintf(stderr, "grant: %s\n", a_ec.message().c_str());
			a_done();
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
	boost::system::error_code ec;
#ifdef ALEXAAGENT_TLS
	boost::asio::ssl::context tls(boost::asio::ssl::context::tlsv12);
	tls.use_private_key_file("configuration/server.key", boost::asio::ssl::context::pem);
	tls.use_certificate_chain_file("configuration/server.crt");
	nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls);
	if (server.listen_and_serve(ec, tls, "localhost", "3000", true)) throw std::runtime_error(ec.message());
#else
	if (server.listen_and_serve(ec, "localhost", "3001", true)) throw std::runtime_error(ec.message());
#endif
	scheduler.reset(new t_scheduler(*server.io_services().front()));
	boost::asio::signal_set signals(*server.io_services().front(), SIGINT);
	signals.async_wait([&](auto, auto a_signal)
	{
		std::fprintf(stderr, "\ncaught signal: %d\n", a_signal);
		scheduler->f_shutdown(std::bind(&nghttp2::asio_http2::server::http2::stop, std::ref(server)));
		if (wsthread.joinable()) wsserver.stop();
	});
	{
		std::string token;
		std::ifstream("session/token") >> token;
		if (!token.empty()) f_refresh(token);
	}
	server.join();
	wsthread.join();
	std::fprintf(stderr, "server stopped.\n");
	return 0;
}
