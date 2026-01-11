#ifndef WEBAUDIOSOUND_H
#define WEBAUDIOSOUND_H

#ifdef __EMSCRIPTEN__

#include "i_sound.h"

class WebAudioSoundRenderer : public SoundRenderer
{
public:
	WebAudioSoundRenderer();
	~WebAudioSoundRenderer();

	bool IsValid();
	void SetSfxVolume(float volume);
	void SetMusicVolume(float volume);
	SoundHandle LoadSound(BYTE *sfxdata, int length);
	SoundHandle LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend = -1);
	void UnloadSound(SoundHandle sfx);
	unsigned int GetMSLength(SoundHandle sfx);
	unsigned int GetSampleLength(SoundHandle sfx);
	float GetOutputRate();

	SoundStream *CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata);
	SoundStream *OpenStream(const char *filename, int flags, int offset, int length);

	FISoundChannel *StartSound(SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan);
	FISoundChannel *StartSound3D(SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan);
	void StopChannel(FISoundChannel *chan);
	void ChannelVolume(FISoundChannel *chan, float volume);
	void MarkStartTime(FISoundChannel *chan);
	unsigned int GetPosition(FISoundChannel *chan);
	float GetAudibility(FISoundChannel *chan);
	void Sync(bool sync);
	void SetSfxPaused(bool paused, int slot);
	void SetInactive(EInactiveState inactive);
	void UpdateSoundParams3D(SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel);
	void UpdateListener(SoundListener *listener);
	void UpdateSounds();
	void PrintStatus();
	void PrintDriversList();

private:
	struct Impl;
	Impl *P;
};

#endif // __EMSCRIPTEN__

#endif // WEBAUDIOSOUND_H
