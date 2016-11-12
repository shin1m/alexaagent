#ifndef ALEXAAGENT__AUDIO_H
#define ALEXAAGENT__AUDIO_H

#include <algorithm>
#include <functional>
#include <vector>
#include <AL/al.h>
#include <AL/alc.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class t_audio_decoder;

class t_audio_source
{
	friend class t_audio_decoder;

protected:
	AVFormatContext* v_format = nullptr;
	int v_index = 0;
	AVCodecContext* v_codec = nullptr;

public:
	virtual ~t_audio_source()
	{
		avcodec_free_context(&v_codec);
		avformat_close_input(&v_format);
	}
};

class t_audio_decoder
{
	AVFormatContext* v_format;
	int v_index;
	AVCodecContext* v_codec;
	AVFrame* v_frame = nullptr;
	AVPacket v_packet;

	template<typename T_0, typename T_1>
	T_0 f_convert(T_1 a_x)
	{
		return a_x;
	}
	template<typename T_0>
	T_0 f_convert(int32_t a_x)
	{
		return a_x >> 16;
	}
	template<typename T_0>
	T_0 f_convert(float a_x)
	{
		return ~(~0 << sizeof(T_0) * 8 - 1) * a_x;
	}
	template<typename T_0>
	T_0 f_convert(double a_x)
	{
		return ~(~0 << sizeof(T_0) * 8 - 1) * a_x;
	}
	uint8_t* f_at(nullptr_t, int a_bytes, int a_sample, int a_channel)
	{
		return v_frame->data[0] + a_bytes * (v_frame->channels * a_sample + a_channel);
	}
	uint8_t* f_at(bool, int a_bytes, int a_sample, int a_channel)
	{
		return v_frame->data[a_channel] + a_bytes * a_sample;
	}
	template<typename T_0, typename T_1, typename T_planer, typename T_target>
	void f_write(int a_channels, T_planer a_planer, T_target a_target)
	{
		std::vector<char> data(v_frame->nb_samples * a_channels * sizeof(T_0));
		int bytes = av_get_bytes_per_sample(v_codec->sample_fmt);
		auto p = data.begin();
		for (int i = 0; i < v_frame->nb_samples; ++i) {
			for (int j = 0; j < a_channels; ++j) {
				auto x = f_convert<T_0>(*reinterpret_cast<T_1*>(f_at(a_planer, bytes, i, j)));
				p = std::copy_n(reinterpret_cast<char*>(&x), sizeof(T_0), p);
			}
		}
		a_target(a_channels, sizeof(T_0), data.data(), data.size(), v_codec->sample_rate);
	}
	template<typename T_target>
	void f_decode(T_target a_target, const AVPacket* a_packet)
	{
		int n = avcodec_send_packet(v_codec, a_packet);
		if (n != 0) throw std::runtime_error("avcodec_send_packet: " + std::to_string(n));
		while (true) {
			int n = avcodec_receive_frame(v_codec, v_frame);
			switch (n) {
			case 0:
				break;
			case AVERROR(EAGAIN):
			case AVERROR_EOF:
				return;
			default:
				if (n < 0) throw std::runtime_error("avcodec_receive_frame: " + std::to_string(n));
			}
			int channels = v_codec->channels;
			if (channels < 1) throw std::runtime_error("no channels");
			if (channels > 2) {
				std::fprintf(stderr, "too many channels: %d\n", channels);
				channels = 2;
			}
			switch (v_codec->sample_fmt) {
			case AV_SAMPLE_FMT_U8:
				f_write<char, char>(channels, nullptr, a_target);
				break;
			case AV_SAMPLE_FMT_S16:
				f_write<int16_t, int16_t>(channels, nullptr, a_target);
				break;
			case AV_SAMPLE_FMT_S32:
				f_write<int16_t, int32_t>(channels, nullptr, a_target);
				break;
			case AV_SAMPLE_FMT_FLT:
				f_write<int16_t, float>(channels, nullptr, a_target);
				break;
			case AV_SAMPLE_FMT_DBL:
				f_write<int16_t, double>(channels, nullptr, a_target);
				break;
			case AV_SAMPLE_FMT_U8P:
				f_write<char, char>(channels, true, a_target);
				break;
			case AV_SAMPLE_FMT_S16P:
				f_write<int16_t, int16_t>(channels, true, a_target);
				break;
			case AV_SAMPLE_FMT_S32P:
				f_write<int16_t, int32_t>(channels, true, a_target);
				break;
			case AV_SAMPLE_FMT_FLTP:
				f_write<int16_t, float>(channels, true, a_target);
				break;
			case AV_SAMPLE_FMT_DBLP:
				f_write<int16_t, double>(channels, true, a_target);
				break;
			default:
				throw std::runtime_error("unknown sample format: " + std::to_string(v_codec->sample_fmt));
			}
		}
	}

public:
	t_audio_decoder(t_audio_source& a_source) : v_format(a_source.v_format), v_index(a_source.v_index), v_codec(a_source.v_codec)
	{
		v_frame = av_frame_alloc();
		if (!v_frame) throw std::runtime_error("av_frame_alloc");
		av_init_packet(&v_packet);
		v_packet.data = NULL;
		v_packet.size = 0;
	}
	~t_audio_decoder()
	{
		av_packet_unref(&v_packet);
		av_frame_free(&v_frame);
	}
	template<typename T_target>
	void operator()(T_target a_target)
	{
		while (av_read_frame(v_format, &v_packet) >= 0) {
			if (v_packet.stream_index == v_index) f_decode(a_target, &v_packet);
			av_packet_unref(&v_packet);
		}
		f_decode(a_target, NULL);
	}
};

struct t_url_audio_source : t_audio_source
{
	t_url_audio_source(const char* a_url)
	{
		int n = avformat_open_input(&v_format, a_url, NULL, NULL);
		if (n < 0) throw std::runtime_error("avformat_open_input");
		if (avformat_find_stream_info(v_format, NULL) < 0) throw std::runtime_error("avformat_find_stream_info");
		AVCodec* decoder;
		v_index = av_find_best_stream(v_format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
		if (v_index < 0) throw std::runtime_error("av_find_best_stream");
		v_codec = avcodec_alloc_context3(decoder);
		if (!v_codec) throw std::runtime_error("avcodec_alloc_context3");
		if (avcodec_parameters_to_context(v_codec, v_format->streams[v_index]->codecpar) < 0) throw std::runtime_error("avcodec_parameters_to_context");
		if (avcodec_open2(v_codec, decoder, NULL) < 0) throw std::runtime_error("avcodec_open2");
	}
};

class t_callback_audio_source : public t_audio_source
{
	static int f_read(void* a_opaque, uint8_t* a_p, int a_n)
	{
		return static_cast<t_callback_audio_source*>(a_opaque)->v_read(a_p, a_n);
	}

	std::function<int(uint8_t*, int)> v_read;
	AVIOContext* v_io = nullptr;

public:
	t_callback_audio_source(std::function<int(uint8_t*, int)>&& a_read) : v_read(std::move(a_read))
	{
		v_format = avformat_alloc_context();
		if (!v_format) throw std::runtime_error("avformat_alloc_context");
		const size_t v_size = 4096;
		auto buffer = static_cast<uint8_t*>(av_malloc(v_size));
		if (buffer == NULL) throw std::runtime_error("av_malloc");
		v_io = avio_alloc_context(buffer, v_size, 0, this, f_read, NULL, NULL);
		if (!v_io) throw std::runtime_error("avio_alloc_context");
		v_format->pb = v_io;
		int n = avformat_open_input(&v_format, NULL, NULL, NULL);
		if (n < 0) throw std::runtime_error("avformat_open_input");
		auto decoder = avcodec_find_decoder(AV_CODEC_ID_MP3);
		if (!decoder) throw std::runtime_error("avcodec_find_decoder");
		v_codec = avcodec_alloc_context3(decoder);
		if (!v_codec) throw std::runtime_error("avcodec_alloc_context3");
		if (avcodec_open2(v_codec, decoder, NULL) < 0) throw std::runtime_error("avcodec_open2");
	}
	virtual ~t_callback_audio_source()
	{
		av_freep(&v_io->buffer);
		av_free(v_io);
	}
};

class t_audio_target
{
	static double f_duration(ALuint a_buffer)
	{
		ALint channels;
		alGetBufferi(a_buffer, AL_CHANNELS, &channels);
		ALint bits;
		alGetBufferi(a_buffer, AL_BITS, &bits);
		ALint size;
		alGetBufferi(a_buffer, AL_SIZE, &size);
		ALint frequency;
		alGetBufferi(a_buffer, AL_FREQUENCY, &frequency);
		return size * 8.0 / (channels * bits * frequency);
	}

	ALuint v_source;
	double v_remain = 0.0;
	double v_processed = 0.0;
	ALfloat v_offset = 0.0;

	ALint f_get(ALenum a_name) const
	{
		ALint i;
		alGetSourcei(v_source, a_name, &i);
		return i;
	}
	void f_unqueue(ALuint* a_p, size_t a_n)
	{
		alSourceUnqueueBuffers(v_source, a_n, a_p);
		for (size_t i = 0; i < a_n; ++i) {
			double x = f_duration(a_p[i]);
			v_remain -= x;
			v_processed += x;
		}
	}

public:
	t_audio_target()
	{
		alGenSources(1, &v_source);
	}
	~t_audio_target()
	{
		f_stop();
		alDeleteSources(1, &v_source);
	}
	operator ALuint() const
	{
		return v_source;
	}
	double f_remain() const
	{
		return v_remain;
	}
	double f_offset() const
	{
		return v_processed + v_offset;
	}
	void f_reset()
	{
		v_remain = v_processed = v_offset = 0.0;
	}
	ALint operator()(size_t a_channels, size_t a_bytes, const char* a_p, size_t a_n, size_t a_rate)
	{
		ALint queued = f_get(AL_BUFFERS_QUEUED);
		ALint n = f_get(AL_BUFFERS_PROCESSED);
		ALuint buffer;
		if (n > 1) {
			std::vector<ALuint> buffers(n);
			f_unqueue(buffers.data(), n);
			alDeleteBuffers(--n, buffers.data());
			buffer = buffers.back();
		} if (n > 0) {
			f_unqueue(&buffer, 1);
		} else {
			alGenBuffers(1, &buffer);
		}
		alBufferData(buffer, a_channels == 1 ? (a_bytes == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16) : (a_bytes == 1 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16), a_p, a_n, a_rate);
		alSourceQueueBuffers(v_source, 1, &buffer);
		v_remain += f_duration(buffer);
		ALint state = f_get(AL_SOURCE_STATE);
		if (state != AL_PLAYING) alSourcePlay(v_source);
		alGetSourcef(v_source, AL_SEC_OFFSET, &v_offset);
		return ++queued - n;
	}
	ALint f_flush()
	{
		ALint queued = f_get(AL_BUFFERS_QUEUED);
		ALint n = f_get(AL_BUFFERS_PROCESSED);
		std::vector<ALuint> buffers(n);
		f_unqueue(buffers.data(), n);
		alDeleteBuffers(n, buffers.data());
		alGetSourcef(v_source, AL_SEC_OFFSET, &v_offset);
		return queued - n;
	}
	void f_stop()
	{
		alSourceStop(v_source);
		ALint n = f_get(AL_BUFFERS_QUEUED);
		std::vector<ALuint> buffers(n);
		alSourceUnqueueBuffers(v_source, n, buffers.data());
		alDeleteBuffers(n, buffers.data());
		f_reset();
	}
};

#endif
