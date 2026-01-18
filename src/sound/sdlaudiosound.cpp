#ifndef __EMSCRIPTEN__

#include "sdlaudiosound.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "c_cvars.h"
#include "i_system.h"
#include "m_swap.h"
#include "s_sound.h"
#include "v_text.h"

namespace
{
	static inline int16_t clamp16(int v)
	{
		if (v > 32767) return 32767;
		if (v < -32768) return -32768;
		return (int16_t)v;
	}

	struct SDLSample
	{
		std::vector<int16_t> pcm;
		int rate = 11025;
		int channels = 1;
		int loopStart = 0;
		int loopEnd = -1;

		int frames() const
		{
			if (channels <= 0) return 0;
			return (int)(pcm.size() / channels);
		}
	};

	struct SDLChannel
	{
		SDLSample *sample = nullptr;
		uint32_t pos = 0;
		uint32_t step = 0;
		float volume = 1.0f;
		bool looping = false;
		bool active = false;
		FISoundChannel *owner = nullptr;
	};

	class NullSoundStream final : public SoundStream
	{
	public:
		bool Play(bool /*looping*/, float /*volume*/) override { return false; }
		void Stop() override {}
		void SetVolume(float /*volume*/) override {}
		bool SetPaused(bool /*paused*/) override { return true; }
		unsigned int GetPosition() override { return 0; }
		bool IsEnded() override { return true; }
		FString GetStats() override { return "Null SDL stream"; }
	};

	class CallbackSoundStream final : public SoundStream
	{
	public:
		CallbackSoundStream(SoundStreamCallback cb, int chunkBytes, int flags, int sampleRate, void *ud)
			: Callback(cb), ChunkBytes(std::max(256, chunkBytes)), Flags(flags), SampleRate(sampleRate), Userdata(ud)
		{
			Temp.resize((size_t)ChunkBytes);
		}

		bool Play(bool looping, float volume) override
		{
			Looping = looping;
			Volume = volume;
			Paused = false;
			Ended = false;
			return true;
		}

		void Stop() override
		{
			Ended = true;
		}

		void SetVolume(float volume) override
		{
			Volume = volume;
		}

		bool SetPaused(bool paused) override
		{
			Paused = paused;
			return true;
		}

		unsigned int GetPosition() override
		{
			return Position;
		}

		bool IsEnded() override
		{
			return Ended;
		}

		FString GetStats() override
		{
			FString out;
			out.Format("SDL stream: pos=%u ended=%d paused=%d", Position, Ended ? 1 : 0, Paused ? 1 : 0);
			return out;
		}

		void MixInto(int16_t *out, int frames, int outRate)
		{
			if (Ended || Paused || frames <= 0) return;

			while ((int)StereoQueue.size() < frames * 2 && !Ended)
			{
				if (!Callback) { Ended = true; break; }
				bool ok = Callback(this, Temp.data(), ChunkBytes, Userdata);
				if (!ok)
				{
					if (Looping)
					{
						continue;
					}
					Ended = true;
					break;
				}

				ConvertTempToQueue(outRate);
			}

			const float vol = std::max(0.0f, Volume);
			for (int i = 0; i < frames; i++)
			{
				if ((int)StereoQueue.size() < 2) break;
				int16_t l = StereoQueue.front(); StereoQueue.pop_front();
				int16_t r = StereoQueue.front(); StereoQueue.pop_front();
				int idx = i * 2;
				int accL = out[idx + 0] + (int)(l * vol);
				int accR = out[idx + 1] + (int)(r * vol);
				out[idx + 0] = clamp16(accL);
				out[idx + 1] = clamp16(accR);
				Position++;
			}
		}

	private:
		void ConvertTempToQueue(int outRate)
		{
			const bool isFloat = (Flags & SoundStream::Float) != 0;
			const bool isMono = (Flags & SoundStream::Mono) != 0;
			const bool is8 = (Flags & SoundStream::Bits8) != 0;
			const bool is32 = (Flags & SoundStream::Bits32) != 0;

			int inCh = isMono ? 1 : 2;
			int inRate = (SampleRate > 0) ? SampleRate : outRate;

			int inSamples = 0;
			if (isFloat)
			{
				inSamples = (int)(Temp.size() / sizeof(float));
			}
			else if (is32)
			{
				inSamples = (int)(Temp.size() / sizeof(int32_t));
			}
			else if (is8)
			{
				inSamples = (int)Temp.size();
			}
			else
			{
				inSamples = (int)(Temp.size() / sizeof(int16_t));
			}

			int inFrames = (inCh > 0) ? (inSamples / inCh) : 0;
			if (inFrames <= 0) return;

			uint32_t step = (uint32_t)(((uint64_t)inRate << 16) / (uint64_t)outRate);
			uint32_t pos = 0;

			auto readSample = [&](int frame, int ch) -> float {
				int idx = frame * inCh + ch;
				idx = std::max(0, std::min(idx, inSamples - 1));
				if (isFloat)
				{
					const float *pf = (const float *)Temp.data();
					return pf[idx];
				}
				if (is32)
				{
					const int32_t *pi = (const int32_t *)Temp.data();
					return (float)pi[idx] / 2147483648.0f;
				}
				if (is8)
				{
					const uint8_t *pu = (const uint8_t *)Temp.data();
					return ((int)pu[idx] - 128) / 128.0f;
				}
				{
					const int16_t *ps = (const int16_t *)Temp.data();
					return (float)ps[idx] / 32768.0f;
				}
			};

			int maxOutFrames = (int)((((uint64_t)inFrames << 16) + step - 1) / step);
			maxOutFrames = std::min(maxOutFrames, 8192);

			for (int o = 0; o < maxOutFrames; o++)
			{
				int f = (int)(pos >> 16);
				if (f >= inFrames) break;
				float l = readSample(f, 0);
				float r = (inCh == 2) ? readSample(f, 1) : l;
				int16_t il = clamp16((int)(l * 32767.0f));
				int16_t ir = clamp16((int)(r * 32767.0f));
				StereoQueue.push_back(il);
				StereoQueue.push_back(ir);
				pos += step;
			}
		}

		SoundStreamCallback Callback = nullptr;
		int ChunkBytes = 0;
		int Flags = 0;
		int SampleRate = 44100;
		void *Userdata = nullptr;

		bool Looping = false;
		bool Paused = false;
		bool Ended = false;
		float Volume = 1.0f;
		unsigned int Position = 0;

		std::vector<uint8_t> Temp;
		std::deque<int16_t> StereoQueue;
	};
}

struct SDLAudioSoundRenderer::Impl
{
	SDL_AudioSpec obtained{};
	bool audioOk = false;
	bool useSDL = false;
	bool headless = false;
	float sfxVolume = 1.0f;
	float musicVolume = 1.0f;
	bool sfxPaused = false;
	EInactiveState inactive = INACTIVE_Active;

	std::mutex audioMutex;
	std::atomic<bool> headlessRunning{false};
	std::thread headlessThread;
	std::string fifoPath;
#ifndef _WIN32
	int fifoFd = -1;
#endif

	std::vector<SDLChannel> channels;
	std::vector<FISoundChannel *> ended;

	CallbackSoundStream *musicStream = nullptr;

	void LockAudio()
	{
		if (useSDL)
			SDL_LockAudio();
		else
			audioMutex.lock();
	}

	void UnlockAudio()
	{
		if (useSDL)
			SDL_UnlockAudio();
		else
			audioMutex.unlock();
	}

	void StopHeadless()
	{
		if (!headlessRunning.load())
			return;
		headlessRunning.store(false);
		if (headlessThread.joinable())
			headlessThread.join();
#ifndef _WIN32
		if (fifoFd >= 0)
		{
			close(fifoFd);
			fifoFd = -1;
		}
#endif
	}

	static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len)
	{
		Impl *self = (Impl *)userdata;
		if (!self) return;
		int16_t *out = (int16_t *)stream;
		int frames = len / (int)(sizeof(int16_t) * 2);
		if (frames <= 0) return;

		std::memset(stream, 0, (size_t)len);

		if (self->inactive != INACTIVE_Complete && self->musicStream != nullptr)
		{
			self->musicStream->SetVolume(self->musicVolume);
			self->musicStream->MixInto(out, frames, self->obtained.freq);
		}

		if (self->inactive == INACTIVE_Complete) return;
		if (self->sfxPaused) return;

		const float master = (self->inactive == INACTIVE_Mute) ? 0.0f : self->sfxVolume;
		if (master <= 0.0f) return;

		for (auto &ch : self->channels)
		{
			if (!ch.active || !ch.sample) continue;

			SDLSample *s = ch.sample;
			int sFrames = s->frames();
			if (sFrames <= 0) { ch.active = false; continue; }
			int loopStart = std::max(0, std::min(s->loopStart, sFrames));
			int loopEnd = (s->loopEnd < 0) ? sFrames : std::max(loopStart, std::min(s->loopEnd, sFrames));

			float vol = std::max(0.0f, ch.volume) * master;
			if (vol <= 0.0f) continue;

			for (int i = 0; i < frames; i++)
			{
				int f = (int)(ch.pos >> 16);
				if (f >= sFrames)
				{
					if (ch.looping && loopEnd > loopStart)
					{
						ch.pos = (uint32_t)(loopStart << 16);
						f = loopStart;
					}
					else
					{
						ch.active = false;
						ch.pos = (uint32_t)(sFrames << 16);
						if (ch.owner) self->ended.push_back(ch.owner);
						break;
					}
				}

				int idx = f * s->channels;
				int16_t sl = s->pcm[idx];
				int16_t sr = (s->channels == 2) ? s->pcm[idx + 1] : sl;

				int outIdx = i * 2;
				int accL = out[outIdx + 0] + (int)(sl * vol);
				int accR = out[outIdx + 1] + (int)(sr * vol);
				out[outIdx + 0] = clamp16(accL);
				out[outIdx + 1] = clamp16(accR);

				ch.pos += ch.step;
				int nextF = (int)(ch.pos >> 16);
				if (ch.looping && nextF >= loopEnd && loopEnd > loopStart)
				{
					ch.pos = (uint32_t)(loopStart << 16);
				}
			}
		}
	}

	void EnsureAudio()
	{
		if (audioOk) return;

		if (SDL_WasInit(SDL_INIT_AUDIO) == 0)
		{
			SDL_InitSubSystem(SDL_INIT_AUDIO);
		}

		SDL_AudioSpec want{};
		want.freq = 44100;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		want.callback = &AudioCallback;
		want.userdata = this;

		if (SDL_OpenAudio(&want, &obtained) < 0)
		{
			// In container/headless environments there may be no ALSA device.
			// If requested, run a headless mixer loop that still produces PCM.
			const char *headlessEnv = getenv("DORCH_HEADLESS_AUDIO");
			const char *fifoEnv = getenv("DORCH_AUDIO_FIFO");
			const bool wantHeadless =
				(headlessEnv != nullptr && headlessEnv[0] != '\0' && strcmp(headlessEnv, "0") != 0) ||
				(fifoEnv != nullptr && fifoEnv[0] != '\0');

			if (!wantHeadless)
			{
				Printf(TEXTCOLOR_RED"SDL audio: SDL_OpenAudio failed: %s\n", SDL_GetError());
				audioOk = false;
				return;
			}

			useSDL = false;
			headless = true;
			obtained = want;
			if (obtained.freq <= 0) obtained.freq = 44100;
			channels.resize(32);
			audioOk = true;
			if (fifoEnv != nullptr && fifoEnv[0] != '\0')
			{
				fifoPath = fifoEnv;
			}

			headlessRunning.store(true);
			headlessThread = std::thread([this]() {
				const int frames = (obtained.samples > 0) ? obtained.samples : 1024;
				const int bytesPerFrame = (int)(sizeof(int16_t) * 2);
				const int len = frames * bytesPerFrame;
				std::vector<uint8_t> buffer((size_t)len);
				const int outRate = (obtained.freq > 0) ? obtained.freq : 44100;

				while (headlessRunning.load())
				{
					// Serialize with main thread updates.
					audioMutex.lock();
					AudioCallback(this, buffer.data(), len);
					audioMutex.unlock();

					// Optional: write to FIFO for ffmpeg capture.
#ifndef _WIN32
					if (!fifoPath.empty())
					{
						if (fifoFd < 0)
						{
							fifoFd = open(fifoPath.c_str(), O_RDWR | O_NONBLOCK);
						}
						if (fifoFd >= 0)
						{
							ssize_t w = write(fifoFd, buffer.data(), (size_t)len);
							(void)w;
							if (w < 0 && (errno == EPIPE || errno == EBADF))
							{
								close(fifoFd);
								fifoFd = -1;
							}
						}
					}
#endif

					std::this_thread::sleep_for(std::chrono::microseconds((long long)frames * 1000000LL / outRate));
				}
			});

			Printf("SDL audio: headless mixer started (%d Hz)\n", obtained.freq);
			return;
		}

		useSDL = true;
		channels.resize(32);
		SDL_PauseAudio(0);
		audioOk = true;
		Printf("SDL audio: started (%d Hz)\n", obtained.freq);
	}

	SDLChannel *AllocChannel(FISoundChannel *owner)
	{
		for (auto &c : channels)
		{
			if (!c.active)
			{
				c = SDLChannel{};
				c.active = true;
				c.owner = owner;
				return &c;
			}
		}

		if (!channels.empty())
		{
			if (channels[0].active && channels[0].owner)
			{
				ended.push_back(channels[0].owner);
			}
			channels[0].active = false;
			channels[0] = SDLChannel{};
			channels[0].active = true;
			channels[0].owner = owner;
			return &channels[0];
		}
		return nullptr;
	}
};

SDLAudioSoundRenderer::SDLAudioSoundRenderer()
{
	P = new Impl;
	P->EnsureAudio();
}

SDLAudioSoundRenderer::~SDLAudioSoundRenderer()
{
	if (P)
	{
		if (P->audioOk)
		{
			P->LockAudio();
			for (auto &c : P->channels) c.active = false;
			P->UnlockAudio();
			if (P->useSDL)
			{
				SDL_CloseAudio();
			}
			else if (P->headless)
			{
				P->StopHeadless();
			}
		}
		delete P;
		P = nullptr;
	}
}

bool SDLAudioSoundRenderer::IsValid()
{
	return P != nullptr && P->audioOk;
}

void SDLAudioSoundRenderer::SetSfxVolume(float volume)
{
	if (!P) return;
	P->sfxVolume = std::max(0.0f, std::min(volume, 1.0f));
}

void SDLAudioSoundRenderer::SetMusicVolume(float volume)
{
	if (!P) return;
	P->musicVolume = std::max(0.0f, std::min(volume, 1.0f));
}

SoundHandle SDLAudioSoundRenderer::LoadSound(BYTE * /*sfxdata*/, int /*length*/)
{
	SoundHandle h{};
	h.data = NULL;
	return h;
}

SoundHandle SDLAudioSoundRenderer::LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
	SoundHandle h{};
	if (!P || !P->audioOk || sfxdata == nullptr || length <= 0) { h.data = NULL; return h; }
	if (channels != 1 && channels != 2) { h.data = NULL; return h; }

	SDLSample *s = new SDLSample;
	s->channels = channels;
	s->rate = (frequency > 0) ? frequency : 11025;

	int bytesPerSample = (bits == 16) ? 2 : 1;
	if (bits != 8 && bits != 16)
	{
		delete s;
		h.data = NULL;
		return h;
	}

	int inSamples = length / bytesPerSample;
	int inFrames = inSamples / channels;
	if (inFrames <= 0)
	{
		delete s;
		h.data = NULL;
		return h;
	}

	s->pcm.resize((size_t)inFrames * (size_t)channels);
	if (bits == 8)
	{
		const uint8_t *in = (const uint8_t *)sfxdata;
		for (int i = 0; i < inFrames * channels; i++)
		{
			int v = (int)in[i] - 128;
			s->pcm[(size_t)i] = (int16_t)(v << 8);
		}
	}
	else
	{
		const int16_t *in = (const int16_t *)sfxdata;
		for (int i = 0; i < inFrames * channels; i++)
		{
			s->pcm[(size_t)i] = LittleShort(in[i]);
		}
	}

	s->loopStart = std::max(0, loopstart);
	s->loopEnd = loopend;

	h.data = s;
	return h;
}

void SDLAudioSoundRenderer::UnloadSound(SoundHandle sfx)
{
	if (!sfx.isValid()) return;
	SDLSample *s = (SDLSample *)sfx.data;
	if (!s) return;
	if (P && P->audioOk)
	{
		P->LockAudio();
		for (auto &c : P->channels)
		{
			if (c.active && c.sample == s)
			{
				c.active = false;
				c.sample = nullptr;
				if (c.owner) P->ended.push_back(c.owner);
				c.owner = nullptr;
			}
		}
		P->UnlockAudio();
	}
	delete s;
}

unsigned int SDLAudioSoundRenderer::GetMSLength(SoundHandle sfx)
{
	if (!sfx.isValid()) return 0;
	SDLSample *s = (SDLSample *)sfx.data;
	if (!s || s->rate <= 0) return 0;
	int frames = s->frames();
	if (frames <= 0) return 0;
	return (unsigned int)((frames * 1000) / s->rate);
}

unsigned int SDLAudioSoundRenderer::GetSampleLength(SoundHandle sfx)
{
	if (!sfx.isValid()) return 0;
	SDLSample *s = (SDLSample *)sfx.data;
	if (!s) return 0;
	return (unsigned int)s->frames();
}

float SDLAudioSoundRenderer::GetOutputRate()
{
	if (!P || !P->audioOk) return 11025.0f;
	return (float)P->obtained.freq;
}

SoundStream *SDLAudioSoundRenderer::CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
{
	if (!P || !P->audioOk)
	{
		return new NullSoundStream();
	}

	P->LockAudio();
	// Only one music stream for now.
	if (P->musicStream != nullptr)
	{
		delete P->musicStream;
		P->musicStream = nullptr;
	}
	P->musicStream = new CallbackSoundStream(callback, buffbytes, flags, samplerate, userdata);
	P->UnlockAudio();
	return P->musicStream;
}

SoundStream *SDLAudioSoundRenderer::OpenStream(const char * /*filename*/, int /*flags*/, int /*offset*/, int /*length*/)
{
	return new NullSoundStream;
}

long SDLAudioSoundRenderer::PlayStream(SoundStream *stream, int volume)
{
	if (!P || !P->audioOk || stream == nullptr) return -1;
	float vol = std::max(0.0f, std::min(volume / 127.0f, 1.0f));
	return stream->Play(true, vol) ? 0 : -1;
}

void SDLAudioSoundRenderer::StopStream(SoundStream *stream)
{
	if (!P || !P->audioOk || stream == nullptr) return;
	P->LockAudio();
	if (P->musicStream == stream) P->musicStream = nullptr;
	P->UnlockAudio();
	stream->Stop();
}

FISoundChannel *SDLAudioSoundRenderer::StartSound(SoundHandle sfx, float vol, int /*pitch*/, int chanflags, FISoundChannel *reuse_chan)
{
	if (!P || !P->audioOk || !sfx.isValid()) return NULL;

	FISoundChannel *ichan = reuse_chan;
	if (ichan == NULL)
	{
		ichan = S_GetChannel(NULL);
	}

	SDLSample *s = (SDLSample *)sfx.data;
	if (!s || s->frames() <= 0)
	{
		ichan->SysChannel = NULL;
		return ichan;
	}

	P->LockAudio();
	SDLChannel *c = P->AllocChannel(ichan);
	if (!c)
	{
		P->UnlockAudio();
		ichan->SysChannel = NULL;
		return ichan;
	}

	c->sample = s;
	c->pos = 0;
	c->volume = std::max(0.0f, vol);
	c->looping = (chanflags & SNDF_LOOP) != 0;
	int outRate = (P->obtained.freq > 0) ? P->obtained.freq : 44100;
	int inRate = std::max(1, s->rate);
	c->step = (uint32_t)(((uint64_t)inRate << 16) / (uint64_t)outRate);
	ichan->SysChannel = c;
	P->UnlockAudio();

	return ichan;
}

FISoundChannel *SDLAudioSoundRenderer::StartSound3D(SoundHandle sfx, SoundListener * /*listener*/, float vol, FRolloffInfo * /*rolloff*/, float /*distscale*/, int pitch, int /*priority*/, const FVector3 & /*pos*/, const FVector3 & /*vel*/, int /*channum*/, int chanflags, FISoundChannel *reuse_chan)
{
	return StartSound(sfx, vol, pitch, chanflags, reuse_chan);
}

void SDLAudioSoundRenderer::StopChannel(FISoundChannel *chan)
{
	if (!P || !P->audioOk || chan == nullptr) return;
	P->LockAudio();
	SDLChannel *c = (SDLChannel *)chan->SysChannel;
	if (c)
	{
		c->active = false;
		c->sample = nullptr;
		c->owner = nullptr;
	}
	chan->SysChannel = NULL;
	P->UnlockAudio();
}

void SDLAudioSoundRenderer::ChannelVolume(FISoundChannel *chan, float volume)
{
	if (!P || !P->audioOk || chan == nullptr) return;
	P->LockAudio();
	SDLChannel *c = (SDLChannel *)chan->SysChannel;
	if (c) c->volume = std::max(0.0f, volume);
	P->UnlockAudio();
}

void SDLAudioSoundRenderer::MarkStartTime(FISoundChannel * /*chan*/)
{
}

unsigned int SDLAudioSoundRenderer::GetPosition(FISoundChannel *chan)
{
	if (!P || !P->audioOk || chan == nullptr) return 0;
	P->LockAudio();
	SDLChannel *c = (SDLChannel *)chan->SysChannel;
	unsigned int pos = c ? (unsigned int)(c->pos >> 16) : 0;
	P->UnlockAudio();
	return pos;
}

float SDLAudioSoundRenderer::GetAudibility(FISoundChannel *chan)
{
	if (!P || !P->audioOk || chan == nullptr) return 0.0f;
	P->LockAudio();
	SDLChannel *c = (SDLChannel *)chan->SysChannel;
	float a = c ? c->volume : 0.0f;
	P->UnlockAudio();
	return a;
}

void SDLAudioSoundRenderer::Sync(bool /*sync*/)
{
}

void SDLAudioSoundRenderer::SetSfxPaused(bool paused, int /*slot*/)
{
	if (!P) return;
	P->LockAudio();
	P->sfxPaused = paused;
	P->UnlockAudio();
}

void SDLAudioSoundRenderer::SetInactive(EInactiveState inactive)
{
	if (!P) return;
	P->LockAudio();
	P->inactive = inactive;
	P->UnlockAudio();
}

void SDLAudioSoundRenderer::UpdateSoundParams3D(SoundListener * /*listener*/, FISoundChannel * /*chan*/, bool /*areasound*/, const FVector3 & /*pos*/, const FVector3 & /*vel*/)
{
}

void SDLAudioSoundRenderer::UpdateListener(SoundListener * /*listener*/)
{
}

void SDLAudioSoundRenderer::UpdateSounds()
{
	if (!P || !P->audioOk) return;

	std::vector<FISoundChannel *> ended;
	P->LockAudio();
	ended.swap(P->ended);
	P->UnlockAudio();

	for (auto *c : ended)
	{
		if (c) S_ChannelEnded(c);
	}
}

void SDLAudioSoundRenderer::PrintStatus()
{
	if (!P || !P->audioOk)
	{
		Printf(TEXTCOLOR_RED"SDL sound not initialized.\n");
		return;
	}
	Printf("SDL sound active (%d Hz).\n", P->obtained.freq);
}

void SDLAudioSoundRenderer::PrintDriversList()
{
	Printf("SDL audio backend: no driver listing.\n");
}

FString SDLAudioSoundRenderer::GatherStats()
{
	if (!P || !P->audioOk) return "SDL sound inactive.";
	FString out;
	out.Format("SDL sound: rate=%d channels=%zu", P->obtained.freq, P->channels.size());
	return out;
}

#endif // __EMSCRIPTEN__
