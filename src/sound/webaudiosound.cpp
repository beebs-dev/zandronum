#ifdef __EMSCRIPTEN__

#include "webaudiosound.h"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

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

	struct WebSample
	{
		std::vector<int16_t> pcm;
		int rate = 11025;
		int channels = 1; // 1 or 2
		int loopStart = 0; // in frames
		int loopEnd = -1;  // in frames, -1 = end

		int frames() const
		{
			if (channels <= 0) return 0;
			return (int)(pcm.size() / channels);
		}
	};

	struct WebChannel
	{
		WebSample *sample = nullptr;
		uint32_t pos = 0;  // 16.16 fixed, in frames
		uint32_t step = 0; // 16.16 fixed frames per output frame
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
		FString GetStats() override { return "Null web stream"; }
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
			out.Format("WebAudio stream: pos=%u ended=%d paused=%d", Position, Ended ? 1 : 0, Paused ? 1 : 0);
			return out;
		}

		// Called by renderer from audio callback while SDL audio is locked.
		// Fills 'out' with interleaved stereo int16 frames.
		void MixInto(int16_t *out, int frames, int outRate)
		{
			if (Ended || Paused || frames <= 0) return;

			// Simple approach: keep a deque of converted stereo frames.
			while ((int)StereoQueue.size() < frames * 2 && !Ended)
			{
				if (!Callback) { Ended = true; break; }
				bool ok = Callback(this, Temp.data(), ChunkBytes, Userdata);
				if (!ok)
				{
					if (Looping)
					{
						// Best-effort looping: just keep asking the callback.
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

			// Determine input sample count.
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

			// Resample ratio.
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

			// Convert to stereo int16 at outRate.
			// Limit work to a reasonable number of frames.
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

struct WebAudioSoundRenderer::Impl
{
	SDL_AudioSpec obtained{};
	bool audioOk = false;
	float sfxVolume = 1.0f;
	float musicVolume = 1.0f;
	bool sfxPaused = false;
	EInactiveState inactive = INACTIVE_Active;

	std::vector<WebChannel> channels;
	std::vector<FISoundChannel *> ended;

	CallbackSoundStream *musicStream = nullptr;

	static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len)
	{
		Impl *self = (Impl *)userdata;
		if (!self) return;
		int16_t *out = (int16_t *)stream;
		int frames = len / (int)(sizeof(int16_t) * 2);
		if (frames <= 0) return;

		// Clear output.
		std::memset(stream, 0, (size_t)len);

		// Mix music first.
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

			WebSample *s = ch.sample;
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
			Printf(TEXTCOLOR_RED"WebAudio: SDL_OpenAudio failed: %s\n", SDL_GetError());
			audioOk = false;
			return;
		}

		channels.resize(32);
		SDL_PauseAudio(0);
		audioOk = true;
		Printf("WebAudio: SDL audio started (%d Hz)\n", obtained.freq);
	}

	WebChannel *AllocChannel(FISoundChannel *owner)
	{
		for (auto &c : channels)
		{
			if (!c.active)
			{
				c = WebChannel{};
				c.active = true;
				c.owner = owner;
				return &c;
			}
		}
		// Steal first channel if all busy.
		if (!channels.empty())
		{
			if (channels[0].active && channels[0].owner)
			{
				// Notify the engine that this channel was evicted.
				ended.push_back(channels[0].owner);
			}
			channels[0].active = false;
			channels[0] = WebChannel{};
			channels[0].active = true;
			channels[0].owner = owner;
			return &channels[0];
		}
		return nullptr;
	}
};

WebAudioSoundRenderer::WebAudioSoundRenderer()
{
	P = new Impl;
	P->EnsureAudio();
}

WebAudioSoundRenderer::~WebAudioSoundRenderer()
{
	if (P)
	{
		if (P->audioOk)
		{
			SDL_LockAudio();
			for (auto &c : P->channels) c.active = false;
			SDL_UnlockAudio();
			SDL_CloseAudio();
		}
		delete P;
		P = nullptr;
	}
}

bool WebAudioSoundRenderer::IsValid()
{
	return P != nullptr && P->audioOk;
}

void WebAudioSoundRenderer::SetSfxVolume(float volume)
{
	if (!P) return;
	P->sfxVolume = std::max(0.0f, std::min(volume, 1.0f));
}

void WebAudioSoundRenderer::SetMusicVolume(float volume)
{
	if (!P) return;
	P->musicVolume = std::max(0.0f, std::min(volume, 1.0f));
}

SoundHandle WebAudioSoundRenderer::LoadSound(BYTE * /*sfxdata*/, int /*length*/)
{
	// Only raw/DMX paths are supported right now.
	SoundHandle h{};
	h.data = NULL;
	return h;
}

SoundHandle WebAudioSoundRenderer::LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
	SoundHandle h{};
	if (!P || !P->audioOk || sfxdata == nullptr || length <= 0) { h.data = NULL; return h; }
	if (channels != 1 && channels != 2) { h.data = NULL; return h; }

	WebSample *s = new WebSample;
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

void WebAudioSoundRenderer::UnloadSound(SoundHandle sfx)
{
	if (!sfx.isValid()) return;
	WebSample *s = (WebSample *)sfx.data;
	if (!s) return;
	if (P && P->audioOk)
	{
		SDL_LockAudio();
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
		SDL_UnlockAudio();
	}
	delete s;
}

unsigned int WebAudioSoundRenderer::GetMSLength(SoundHandle sfx)
{
	if (!sfx.isValid()) return 0;
	WebSample *s = (WebSample *)sfx.data;
	if (!s || s->rate <= 0) return 0;
	return (unsigned int)((uint64_t)s->frames() * 1000ull / (uint64_t)s->rate);
}

unsigned int WebAudioSoundRenderer::GetSampleLength(SoundHandle sfx)
{
	if (!sfx.isValid()) return 0;
	WebSample *s = (WebSample *)sfx.data;
	return (unsigned int)(s ? s->frames() : 0);
}

float WebAudioSoundRenderer::GetOutputRate()
{
	return (P && P->audioOk) ? (float)P->obtained.freq : 44100.0f;
}

SoundStream *WebAudioSoundRenderer::CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
{
	if (!P || !P->audioOk)
	{
		return new NullSoundStream();
	}

	SDL_LockAudio();
	// Only one music stream for now.
	if (P->musicStream != nullptr)
	{
		delete P->musicStream;
		P->musicStream = nullptr;
	}
	P->musicStream = new CallbackSoundStream(callback, buffbytes, flags, samplerate, userdata);
	SDL_UnlockAudio();
	return P->musicStream;
}

SoundStream *WebAudioSoundRenderer::OpenStream(const char * /*filename*/, int /*flags*/, int /*offset*/, int /*length*/)
{
	// File/codec decoding is not implemented for the web build yet.
	return new NullSoundStream();
}

FISoundChannel *WebAudioSoundRenderer::StartSound(SoundHandle sfx, float vol, int /*pitch*/, int chanflags, FISoundChannel *reuse_chan)
{
	if (!P || !P->audioOk || !sfx.isValid()) return NULL;

	FISoundChannel *ichan = reuse_chan;
	if (ichan == NULL)
	{
		ichan = S_GetChannel(NULL);
	}

	WebSample *s = (WebSample *)sfx.data;
	if (!s || s->frames() <= 0)
	{
		ichan->SysChannel = NULL;
		return ichan;
	}

	SDL_LockAudio();
	WebChannel *c = P->AllocChannel(ichan);
	if (!c)
	{
		SDL_UnlockAudio();
		ichan->SysChannel = NULL;
		return ichan;
	}
	c->sample = s;
	c->volume = vol;
	c->looping = (chanflags & SNDF_LOOP) != 0;
	c->pos = 0;
	c->step = (uint32_t)(((uint64_t)s->rate << 16) / (uint64_t)P->obtained.freq);
	ichan->SysChannel = c;
	SDL_UnlockAudio();

	return ichan;
}

FISoundChannel *WebAudioSoundRenderer::StartSound3D(SoundHandle sfx, SoundListener * /*listener*/, float vol, FRolloffInfo * /*rolloff*/, float /*distscale*/, int pitch, int /*priority*/, const FVector3 & /*pos*/, const FVector3 & /*vel*/, int /*channum*/, int chanflags, FISoundChannel *reuse_chan)
{
	return StartSound(sfx, vol, pitch, chanflags, reuse_chan);
}

void WebAudioSoundRenderer::StopChannel(FISoundChannel *chan)
{
	if (!P || !P->audioOk || !chan) return;
	SDL_LockAudio();
	WebChannel *c = (WebChannel *)chan->SysChannel;
	if (c)
	{
		c->active = false;
		c->sample = nullptr;
		c->owner = nullptr;
	}
	chan->SysChannel = NULL;
	SDL_UnlockAudio();
}

void WebAudioSoundRenderer::ChannelVolume(FISoundChannel *chan, float volume)
{
	if (!P || !P->audioOk || !chan) return;
	SDL_LockAudio();
	WebChannel *c = (WebChannel *)chan->SysChannel;
	if (c) c->volume = volume;
	SDL_UnlockAudio();
}

void WebAudioSoundRenderer::MarkStartTime(FISoundChannel * /*chan*/)
{
	// Not implemented.
}

unsigned int WebAudioSoundRenderer::GetPosition(FISoundChannel *chan)
{
	if (!P || !P->audioOk || !chan) return 0;
	SDL_LockAudio();
	WebChannel *c = (WebChannel *)chan->SysChannel;
	unsigned int pos = 0;
	if (c) pos = (unsigned int)(c->pos >> 16);
	SDL_UnlockAudio();
	return pos;
}

float WebAudioSoundRenderer::GetAudibility(FISoundChannel *chan)
{
	if (!P || !P->audioOk || !chan) return 0.0f;
	SDL_LockAudio();
	WebChannel *c = (WebChannel *)chan->SysChannel;
	float v = c ? c->volume : 0.0f;
	SDL_UnlockAudio();
	return v;
}

void WebAudioSoundRenderer::Sync(bool /*sync*/)
{
	// Not implemented.
}

void WebAudioSoundRenderer::SetSfxPaused(bool paused, int /*slot*/)
{
	if (!P) return;
	SDL_LockAudio();
	P->sfxPaused = paused;
	SDL_UnlockAudio();
}

void WebAudioSoundRenderer::SetInactive(EInactiveState inactive)
{
	if (!P) return;
	SDL_LockAudio();
	P->inactive = inactive;
	SDL_UnlockAudio();
}

void WebAudioSoundRenderer::UpdateSoundParams3D(SoundListener * /*listener*/, FISoundChannel *chan, bool /*areasound*/, const FVector3 & /*pos*/, const FVector3 & /*vel*/)
{
	// Minimal: keep existing volume. Positional audio not implemented.
	(void)chan;
}

void WebAudioSoundRenderer::UpdateListener(SoundListener * /*listener*/)
{
	// No-op.
}

void WebAudioSoundRenderer::UpdateSounds()
{
	if (!P || !P->audioOk) return;

	std::vector<FISoundChannel *> ended;
	SDL_LockAudio();
	ended.swap(P->ended);
	SDL_UnlockAudio();

	for (auto *c : ended)
	{
		if (c) S_ChannelEnded(c);
	}
}

void WebAudioSoundRenderer::PrintStatus()
{
	if (!P || !P->audioOk)
	{
		Printf(TEXTCOLOR_RED"WebAudio sound not initialized.\n");
		return;
	}
	Printf("WebAudio sound active (%d Hz).\n", P->obtained.freq);
}

void WebAudioSoundRenderer::PrintDriversList()
{
	Printf("WebAudio: no selectable drivers in browser build.\n");
}

#endif // __EMSCRIPTEN__
