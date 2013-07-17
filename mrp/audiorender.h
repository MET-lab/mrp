/*
 *  audiorender.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/24/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef AUDIORENDER_H
#define AUDIORENDER_H

#include <iostream>
#include <set>
#include <vector>
#include <pthread.h>
#include "portaudio.h"
#include "synth.h"
#include "osccontroller.h"

using namespace std;

class AudioRender : public OscHandler
{
public:
	AudioRender();
	
	// This should always be called before starting the stream
	void setStreamInfo(PaStream *stream, int numInputChannels, int numOutputChannels, 
					   float sampleRate, vector<int>& channelsToUse);
	
	// Tools for querying the stream or timing status
	PaTime currentTime() { return Pa_GetStreamTime(stream_); }
	PaTime delayTime(PaTime delay) { return (Pa_GetStreamTime(stream_) + delay); }

	double cpuLoad() { return Pa_GetStreamCpuLoad(stream_); }
	
	int numInputChannels() { return numInputChannels_; }
	int numOutputChannels() { return numOutputChannels_; }
	float sampleRate() { return sampleRate_; }		// Ideal (requested) sample rate, as opposed to actual value
	double actualSampleRate();						// These three are part of portaudio's PaStreamInfo
	PaTime inputLatency();
	PaTime outputLatency();
	
	// Set the global output amplitude
	void setGlobalAmplitude(float amp) { globalAmplitude_ = amp; }
	
	// Tools for finding an open audio channel (allocated first-come, first-served)
	// Returns the channel used, or -1 if none available.  Save this channel number
	// for freeing later.  Only one object can use a channel at a time through this mechanism.
	
	pair<int,int> allocateOutputChannel();
	void freeOutputChannel(int channel);		// Return the channel to the pool when finished
	void freeAllOutputChannels();				// Clear the list
	
	// Tools for managing the synth list
	int addSynth(SynthBase *synth);		// Add a new synth to the render list
	int removeSynth(SynthBase *synth);	// Remove a synth from the render list
	void removeAllSynths();				// Clear the render list
	
	// OSC handler routine, for changing calibration settings
	bool oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data);
	void setOscController(OscController *c);	// Override the OscHandler implementation to register our paths
	
	// staticRenderCallback() is called by portaudio, and it in turn passes control to
	// the instance-specific renderCallback, which holds state information about the stream
	// and contains the list of synths to render.
	
	int renderCallback(const void *input, void *output,
					   unsigned long frameCount,
					   const PaStreamCallbackTimeInfo* timeInfo,
					   PaStreamCallbackFlags statusFlags);
	static int staticRenderCallback(
									const void *input, void *output,
									unsigned long frameCount,
									const PaStreamCallbackTimeInfo* timeInfo,
									PaStreamCallbackFlags statusFlags,
									void *userData )
	{
		return ((AudioRender *)userData)->renderCallback(input, output, frameCount, timeInfo, statusFlags);
	}
	
	~AudioRender();
	
private:
	/* Stream information */
	PaStream *stream_;
	int numInputChannels_;
	int numOutputChannels_;
	vector<int> outputChannels_;		// A list of channels we can use for output
	float sampleRate_;

	/* Global amplitude scaler for all outputs */
	float globalAmplitude_;
	
	/* List of output channels, indicating whether each is in use */
	vector<bool> channelsUsed_;
	
	/* List of synth processes to execute on each callback.  Use a map so we can remove specific
	 synths later. */
	set<SynthBase *> synths_;
	
	/* Render list mutex ensures that render list doesn't change while it's being iterated */
	pthread_mutex_t renderMutex_, channelMutex_;
};

#endif // AUDIORENDER_H
