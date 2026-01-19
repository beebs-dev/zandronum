#ifndef FMODSOUND_H
#define FMODSOUND_H

#ifndef NO_SOUND
#include "i_sound.h"
#include "fmod_wrap.h"

// Detect modern FMOD Core (2.x) SDKs (e.g. HTML5) vs legacy FMOD Ex-style SDKs.
// FMOD Core 2.x uses a small version number like 0x00020312 (2.03.12).
// FMOD Ex 4.x uses values like 0x00043800.
#ifndef ZANDRONUM_FMOD_CORE2
#if defined(FMOD_VERSION) && (FMOD_VERSION >= 0x00020000) && (FMOD_VERSION < 0x00030000)
#define ZANDRONUM_FMOD_CORE2 1
#else
#define ZANDRONUM_FMOD_CORE2 0
#endif
#endif

// FMOD Core API 2.x uses F_CALL for calling convention and ChannelControl-based callbacks.
// Older integrations used F_CALLBACK and Channel-based callback typedefs.
#ifndef F_CALLBACK
#define F_CALLBACK F_CALL
#endif

// FMOD Ex had a driver capability type; FMOD Core 2.x removed it.
// Keep the existing interface compiling; caps will be treated as 0 on modern SDKs.
#if ZANDRONUM_FMOD_CORE2 && !defined(FMOD_CAPS_HARDWARE)
typedef unsigned int FMOD_CAPS;
#endif

class FMODSoundRenderer : public SoundRenderer
{
public:
	FMODSoundRenderer ();
	~FMODSoundRenderer ();
	bool IsValid ();

	void SetSfxVolume (float volume);
	void SetMusicVolume (float volume);
	SoundHandle LoadSound(BYTE *sfxdata, int length);
	SoundHandle LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend = -1);
	void UnloadSound (SoundHandle sfx);
	unsigned int GetMSLength(SoundHandle sfx);
	unsigned int GetSampleLength(SoundHandle sfx);
	float GetOutputRate();

	// Streaming sounds.
	SoundStream *CreateStream (SoundStreamCallback callback, int buffsamples, int flags, int samplerate, void *userdata);
	SoundStream *OpenStream (const char *filename, int flags, int offset, int length);
	long PlayStream (SoundStream *stream, int volume);
	void StopStream (SoundStream *stream);

	// Starts a sound.
	FISoundChannel *StartSound (SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan);
	FISoundChannel *StartSound3D (SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan);

	// Stops a sound channel.
	void StopChannel (FISoundChannel *chan);

	// Changes a channel's volume.
	void ChannelVolume (FISoundChannel *chan, float volume);

	// Marks a channel's start time without actually playing it.
	void MarkStartTime (FISoundChannel *chan);

	// Returns position of sound on this channel, in samples.
	unsigned int GetPosition(FISoundChannel *chan);

	// Gets a channel's audibility (real volume).
	float GetAudibility(FISoundChannel *chan);

	// Synchronizes following sound startups.
	void Sync (bool sync);

	// Pauses or resumes all sound effect channels.
	void SetSfxPaused (bool paused, int slot);

	// Pauses or resumes *every* channel, including environmental reverb.
	void SetInactive (EInactiveState inactive);

	// Updates the position of a sound channel.
	void UpdateSoundParams3D (SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel);

	void UpdateListener (SoundListener *listener);
	void UpdateSounds ();

	void PrintStatus ();
	void PrintDriversList ();
	FString GatherStats ();
	short *DecodeSample(int outlen, const void *coded, int sizebytes, ECodecType type);

	void DrawWaveDebug(int mode);

private:
	DWORD ActiveFMODVersion;
	int SFXPaused;
	bool InitSuccess;
	bool DSPLocked;
	QWORD_UNION DSPClock;
	int OutputRate;

#if ZANDRONUM_FMOD_CORE2
	static FMOD_RESULT F_CALLBACK ChannelCallback(FMOD_CHANNELCONTROL *channelcontrol, FMOD_CHANNELCONTROL_TYPE controltype, FMOD_CHANNELCONTROL_CALLBACK_TYPE type, void *data1, void *data2);
	static float F_CALLBACK RolloffCallback(FMOD_CHANNELCONTROL *channelcontrol, float distance);
#else
	static FMOD_RESULT F_CALLBACK ChannelCallback(FMOD_CHANNEL *channel, FMOD_CHANNEL_CALLBACKTYPE type, void *data1, void *data2);
	static float F_CALLBACK RolloffCallback(FMOD_CHANNEL *channel, float distance);
#endif

	bool HandleChannelDelay(FMOD::Channel *chan, FISoundChannel *reuse_chan, int flags, float freq) const;
	FISoundChannel *CommonChannelSetup(FMOD::Channel *chan, FISoundChannel *reuse_chan) const;
	FMOD_MODE SetChanHeadSettings(SoundListener *listener, FMOD::Channel *chan, const FVector3 &pos, bool areasound, FMOD_MODE oldmode) const;

	bool ReconnectSFXReverbUnit();
	void InitCreateSoundExInfo(FMOD_CREATESOUNDEXINFO *exinfo) const;
	FMOD_RESULT SetSystemReverbProperties(const REVERB_PROPERTIES *props);

	bool Init ();
	void Shutdown ();
	void DumpDriverCaps(FMOD_CAPS caps, int minfrequency, int maxfrequency);

	int DrawChannelGroupOutput(FMOD::ChannelGroup *group, float *wavearray, int width, int height, int y, int mode);
	int DrawSystemOutput(float *wavearray, int width, int height, int y, int mode);

	int DrawChannelGroupWaveData(FMOD::ChannelGroup *group, float *wavearray, int width, int height, int y, bool skip);
	int DrawSystemWaveData(float *wavearray, int width, int height, int y, bool skip);
	void DrawWave(float *wavearray, int x, int y, int width, int height);

	int DrawChannelGroupSpectrum(FMOD::ChannelGroup *group, float *wavearray, int width, int height, int y, bool skip);
	int DrawSystemSpectrum(float *wavearray, int width, int height, int y, bool skip);
	void DrawSpectrum(float *spectrumarray, int x, int y, int width, int height);

	typedef char spk[4];
	static const spk SpeakerNames4[4], SpeakerNamesMore[8];
	void DrawSpeakerLabels(const spk *labels, int y, int width, int count);

	FMOD::System *Sys;
	FMOD::ChannelGroup *SfxGroup, *PausableSfx;
	FMOD::ChannelGroup *MusicGroup;
	FMOD::DSP *WaterLP, *WaterReverb;
	FMOD::DSPConnection *SfxConnection;
	FMOD::DSPConnection *ChannelGroupTargetUnitOutput;
	FMOD::DSP *ChannelGroupTargetUnit;
	FMOD::DSP *SfxReverbPlaceholder;
	bool SfxReverbHooked;
	float LastWaterLP;
	unsigned int OutputPlugin;

	// Just for snd_status display
	int Driver_MinFrequency;
	int Driver_MaxFrequency;
	FMOD_CAPS Driver_Caps;

	friend class FMODStreamCapsule;
};

#endif
#endif //NO_SOUND
