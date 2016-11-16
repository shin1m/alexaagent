#include <nghttp2/asio_http2_server.h>
#include <Simple-WebSocket-Server/server_wss.hpp>

#include "agent.h"

template<typename T_server>
void f_web_socket(t_agent* a_agent, std::shared_ptr<T_server> a_server, std::function<void()> a_ready)
{
	std::map<std::string, std::function<void(const picojson::value&)>> handlers{
		{"hello", [&](auto)
		{
			a_agent->v_state_changed();
			a_agent->v_options_changed();
		}},
		{"connect", [&](auto)
		{
			a_agent->f_connect();
		}},
		{"disconnect", [&](auto)
		{
			a_agent->f_disconnect();
		}},
		{"capture.threshold", [&](auto a_x)
		{
			a_agent->f_capture_threshold(a_x.template get<double>());
		}},
		{"capture.auto", [&](auto a_x)
		{
			a_agent->f_capture_auto(a_x.template get<bool>());
		}},
		{"capture.force", [&](auto a_x)
		{
			a_agent->f_capture_force(a_x.template get<bool>());
		}},
		{"alerts.duration", [&](auto a_x)
		{
			a_agent->f_alerts_duration(std::min(static_cast<size_t>(a_x.template get<double>()), size_t(3600)));
		}},
		{"alerts.stop", [&](auto a_x)
		{
			a_agent->f_alerts_stop(a_x.template get<std::string>());
		}},
		{"content.background", [&](auto a_x)
		{
			a_agent->f_content_can_play_in_background(a_x.template get<bool>());
		}},
		{"speaker.volume", [&](auto a_x)
		{
			a_agent->f_speaker_volume(a_x.template get<double>());
		}},
		{"speaker.muted", [&](auto a_x)
		{
			a_agent->f_speaker_muted(a_x.template get<bool>());
		}},
		{"playback.play", [&](auto)
		{
			a_agent->f_playback_play();
		}},
		{"playback.pause", [&](auto)
		{
			a_agent->f_playback_pause();
		}},
		{"playback.next", [&](auto)
		{
			a_agent->f_playback_next();
		}},
		{"playback.previous", [&](auto)
		{
			a_agent->f_playback_previous();
		}}
	};
	auto& ws = a_server->endpoint["^/session$"];
	ws.onmessage = a_agent->f_scheduler().wrap([&](auto a_connection, auto a_message)
	{
		auto s = a_message->string();
		if (a_agent->v_log) a_agent->v_log(e_severity__TRACE) << "websocket on message: " << s << std::endl;
		picojson::value message;
		picojson::parse(message, s.begin(), s.end(), nullptr);
		try {
			for (auto& x : message.get<picojson::value::object>()) handlers.at(x.first)(x.second);
		} catch (std::exception& e) {
			if (a_agent->v_log) a_agent->v_log(e_severity__ERROR) << "websocket failed: " << e.what() << std::endl;
		}
	});
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
	a_agent->v_capture = [&]
	{
		send("capture", {
			{"integral", picojson::value(static_cast<double>(a_agent->f_capture_integral()))},
			{"busy", picojson::value(a_agent->f_capture_busy())}
		});
	};
	a_agent->v_state_changed = [&]
	{
		picojson::value::array alerts;
		for (auto& x : a_agent->f_alerts()) alerts.push_back(picojson::value(picojson::value::object{
			{"token", picojson::value(x.first)},
			{"type", picojson::value(x.second.f_type())},
			{"at", picojson::value(x.second.f_at())},
			{"active", picojson::value(x.second.f_active())}
		}));
		send("state_changed", {
			{"online", picojson::value(a_agent->f_online())},
			{"dialog", picojson::value(picojson::value::object{
				{"active", picojson::value(a_agent->f_dialog_active())},
				{"playing", picojson::value(a_agent->f_dialog_playing())},
				{"expecting_speech", picojson::value(a_agent->f_expecting_speech())}
			})},
			{"alerts", picojson::value(std::move(alerts))},
			{"content", picojson::value(picojson::value::object{
				{"playing", picojson::value(a_agent->f_content_playing())}
			})}
		});
	};
	a_agent->v_options_changed = [&]
	{
		send("options_changed", {
			{"alerts", picojson::value(picojson::value::object{
				{"duration", picojson::value(static_cast<double>(a_agent->f_alerts_duration()))}
			})},
			{"content", picojson::value(picojson::value::object{
				{"can_play_in_background", picojson::value(a_agent->f_content_can_play_in_background())}
			})},
			{"speaker", picojson::value(picojson::value::object{
				{"volume", picojson::value(static_cast<double>(a_agent->f_speaker_volume()))},
				{"muted", picojson::value(a_agent->f_speaker_muted())}
			})},
			{"capture", picojson::value(picojson::value::object{
				{"threshold", picojson::value(static_cast<double>(a_agent->f_capture_threshold()))},
				{"auto", picojson::value(a_agent->f_capture_auto())},
				{"force", picojson::value(a_agent->f_capture_force())}
			})}
		});
	};
	a_agent->f_scheduler().dispatch(a_ready);
	a_server->start();
}

struct t_null_buffer : std::streambuf
{
	virtual int overflow(int a_c)
	{
		return a_c;
	}
};

int main(int argc, char* argv[])
{
	auto severity = e_severity__INFORMATION;
	for (int i = 1; i < argc; ++i) {
		const char* p = argv[i];
		if (p[0] != '-' || p[1] != '-') continue;
		if (std::strncmp(p + 2, "verbose", 7) == 0) {
			severity = e_severity__TRACE;
			if (p[9] == '=') std::sscanf(p + 10, "%u", &severity);
		}
	}
	t_null_buffer null_buffer;
	std::ostream null_stream{&null_buffer};
	auto log = [&](auto a_severity) -> std::ostream&
	{
		return std::ref(a_severity < severity ? null_stream : std::cerr);
	};
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
	std::unique_ptr<t_agent> agent;
	std::thread wsthread;
	std::function<void()> wsstop;
	auto start = [&](auto a_ready)
	{
		agent.reset(new t_agent(*scheduler, log, profile / "client_id"_jss, profile / "client_secret"_jss, sounds));
		auto wsstart = [&](auto a_server)
		{
			wsthread = std::thread(std::bind(f_web_socket<typename decltype(a_server)::element_type>, agent.get(), a_server, a_ready));
			wsstop = [a_server]
			{
				a_server->stop();
			};
		};
		if (service_key.empty())
			wsstart(std::make_shared<SimpleWeb::SocketServer<SimpleWeb::WS>>(wsport));
		else
			wsstart(std::make_shared<SimpleWeb::SocketServer<SimpleWeb::WSS>>(wsport, 1, service / "certificate"_jss, service_key));
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
	auto to_root = nghttp2::asio_http2::server::redirect_handler(302, "/");
	server.handle("/grant", [&](auto& a_request, auto& a_response)
	{
		auto values = f_parse_query_string(a_request.uri().raw_query);
		auto i = values.find("code");
		if (i == values.end()) {
			to_root(a_request, a_response);
		} else {
			scheduler->dispatch(std::bind(start, [&]
			{
				agent->f_grant({
					{"grant_type", "authorization_code"},
					{"code", i->second},
					{"redirect_uri", profile / "redirect_uri"_jss}
				}, [&](auto)
				{
					to_root(a_request, a_response);
				});
			}));
		}
	});
	server.handle("/session", [&](auto&, auto& a_response)
	{
		a_response.write_head(200);
		a_response.end((service_key.empty() ? "ws://" : "wss://") + service_host + ':' + std::to_string(wsport) + "/session");
	});
	auto ok = nghttp2::asio_http2::server::status_handler(200);
	server.handle("/connect", [&](auto& a_request, auto& a_response)
	{
		scheduler->dispatch([&]
		{
			agent->f_connect();
		});
		ok(a_request, a_response);
	});
	server.handle("/disconnect", [&](auto& a_request, auto& a_response)
	{
		scheduler->dispatch([&]
		{
			agent->f_disconnect();
		});
		ok(a_request, a_response);
	});
	boost::asio::ssl::context tls(boost::asio::ssl::context::tlsv12);
	boost::system::error_code ec;
	if (service_key.empty()) {
		if (server.listen_and_serve(ec, service_host, http2port, true)) throw boost::system::system_error(ec);
	} else {
		tls.use_private_key_file(service_key, boost::asio::ssl::context::pem);
		tls.use_certificate_chain_file(service / "certificate"_jss);
		nghttp2::asio_http2::server::configure_tls_context_easy(ec, tls);
		if (server.listen_and_serve(ec, tls, service_host, http2port, true)) throw boost::system::system_error(ec);
	}
	scheduler.reset(new t_scheduler(*server.io_services().front()));
	boost::asio::signal_set signals(*server.io_services().front(), SIGINT);
	signals.async_wait(scheduler->wrap([&](auto, auto a_signal)
	{
		log(e_severity__INFORMATION) << std::endl << "caught signal: " << a_signal << std::endl;
		if (agent) agent->f_disconnect();
		scheduler->f_shutdown([&]
		{
			server.stop();
		});
		if (wsstop) wsstop();
	}));
	{
		std::string token;
		std::ifstream("session/token") >> token;
		if (!token.empty()) scheduler->dispatch(std::bind(start, [&, token]
		{
			agent->f_refresh(token);
		}));
	}
	server.join();
	if (wsthread.joinable()) wsthread.join();
	log(e_severity__INFORMATION) << "server stopped." << std::endl;
	return 0;
}
