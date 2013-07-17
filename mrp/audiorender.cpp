/*
 *  audiorender.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/24/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "audiorender.h"
#include "config.h"

// Initialize the render object
AudioRender::AudioRender()
{
	// Initialize a mutex which prevents changing the render list while it's rendering
	if(pthread_mutex_init(&renderMutex_, NULL) != 0)
	{
		cerr << "Error: Failed to initialize render mutex!\n";
		Pa_Terminate();
		exit(1);		// Can't work without the mutex, so quit
	}
	
	// Similar mutex prevents problems with multi-threaded requests for channel allocation
	if(pthread_mutex_init(&channelMutex_, NULL) != 0)
	{
		cerr << "Error: Failed to initialize channel mutex!\n";
		Pa_Terminate();
		exit(1);		// Can't work without the mutex, so quit
	}	
	
	numOutputChannels_ = 0;
	globalAmplitude_ = 1.0;
}

// Set the basic stream information

void AudioRender::setStreamInfo(PaStream *stream, int numInputChannels, int numOutputChannels, float sampleRate, vector<int>& channelsToUse)
{
	int i;
	
	stream_ = stream;
	numInputChannels_ = numInputChannels;
	numOutputChannels_ = numOutputChannels;
	sampleRate_ = sampleRate;
	
	// Set up the output channel tracking vector, telling us whether each channel is in use
	channelsUsed_.resize(numOutputChannels, false);
	// Right now we don't change the state of any existing channels.  If we eventually want to run
	// setStreamInfo multiple times, consider whether this is the desired behavior.
	
	// Set a list of channels to use
	if(channelsToUse.size() == 0)
	{
		// Use all channels
		outputChannels_.clear();
		for(i = 0; i < numOutputChannels_; i++)
			outputChannels_.push_back(i);
	}
	else
	{
		set<int> alreadySeen;
		
		outputChannels_.clear();
		
		for(i = 0; i < channelsToUse.size(); i++)
		{
			if(channelsToUse[i] >= 0 && channelsToUse[i] < numOutputChannels_ && 
			   alreadySeen.count(channelsToUse[i]) == 0)
			{
				//cout << "adding channel " << channelsToUse[i] << endl;
				outputChannels_.push_back(channelsToUse[i]);
				alreadySeen.insert(channelsToUse[i]);
			}
		}
	}
}

// Useful query methods for current stream

double AudioRender::actualSampleRate()
{
	const PaStreamInfo *streamInfo = Pa_GetStreamInfo(stream_);
	
	if(streamInfo == NULL)	// An error occurred, return 0
		return 0.0;
	return streamInfo->sampleRate;
}

PaTime AudioRender::inputLatency()
{
	const PaStreamInfo *streamInfo = Pa_GetStreamInfo(stream_);
	
	if(streamInfo == NULL)	// An error occurred, return 0
		return (PaTime)0.0;
	return streamInfo->inputLatency;
}

PaTime AudioRender::outputLatency()
{
	const PaStreamInfo *streamInfo = Pa_GetStreamInfo(stream_);
	
	if(streamInfo == NULL)	// An error occurred, return 0
		return (PaTime)0.0;
	return streamInfo->outputLatency;
}

// Tools for managing output channels

// Returns two variables, the first is the index of the audio channel in the render buffer
// The second is the index of which MRP channel to use.  These are often the same, but in the 
// case where we only want to use a subset of a device's channels, the MRP index might be different.
// Returns <-1,-1> on error.

pair<int,int> AudioRender::allocateOutputChannel()
{
	int channelIndex;
	bool foundChannel = false;
	pair<int,int> out;
	
	out.first = out.second = -1;
	
	if(pthread_mutex_lock(&channelMutex_) != 0)	// One at a time for allocating channels
	{
		cerr << "Error: Could not lock mutex in allocateOutputChannel()\n";
		return out;
	}
	
	for(channelIndex = 0; channelIndex < outputChannels_.size(); channelIndex++)
	{
		if(!channelsUsed_[outputChannels_[channelIndex]])
		{
			channelsUsed_[outputChannels_[channelIndex]] = true; // Claim this one
			foundChannel = true;
			break;
		}
	}
	
	if(pthread_mutex_unlock(&channelMutex_) != 0)
	{
		cerr << "Error: Could not unlock mutex in allocateOutputChannel()\n";
		return out;
	}		
	
	if(foundChannel)
	{
		out.first = outputChannels_[channelIndex];
		out.second = channelIndex;
	}
	// else there was no free channel, and out = <-1, -1>
	
	return out;
}

void AudioRender::freeOutputChannel(int channel)		// Return the channel to the pool when finished
{
	if(channel < 0 || channel >= numOutputChannels_)	// sanity check
		return;
	
	if(pthread_mutex_lock(&channelMutex_) != 0)	// One at a time for allocating channels
	{
		cerr << "Error: Could not lock mutex in allocateOutputChannel()\n";
		return;
	}
	
	channelsUsed_[channel] = false;
	
	if(pthread_mutex_unlock(&channelMutex_) != 0)
	{
		cerr << "Error: Could not unlock mutex in allocateOutputChannel()\n";
		return;
	}		
}

void AudioRender::freeAllOutputChannels()				// Clear the list
{
	if(pthread_mutex_lock(&channelMutex_) != 0)	// One at a time for allocating channels
	{
		cerr << "Error: Could not lock mutex in allocateOutputChannel()\n";
		return;
	}
	
	for(int i = 0; i < channelsUsed_.size(); i++)
		channelsUsed_[i] = false;
	
	if(pthread_mutex_unlock(&channelMutex_) != 0)
	{
		cerr << "Error: Could not unlock mutex in allocateOutputChannel()\n";
		return;
	}			
}


// Add a new synth object to the render list.  Returns 0 on success.

int AudioRender::addSynth(SynthBase *synth)
{	
	pair<set<SynthBase*>::iterator,bool> ret;
	
	if(pthread_mutex_lock(&renderMutex_) != 0)	// Acquire lock ensuring we're not actively rendering
	{
		cerr << "Error: Could not lock mutex in addSynth()\n";
		return 1;
	}
	
	ret = synths_.insert(synth);
	
	if(pthread_mutex_unlock(&renderMutex_) != 0)
	{
		cerr << "Error: Could not unlock mutex in addSynth()\n";
		return 1;
	}	
	
	if(ret.second != false)
		return 0;
	else
		return 1;
}

// Remove a synth object from the render list.  Returns 0 on success

int AudioRender::removeSynth(SynthBase *synth)
{
	int ret;
	
	if(pthread_mutex_lock(&renderMutex_) != 0)	// Acquire lock ensuring we're not actively rendering
	{
		cerr << "Error: Could not lock mutex in removeSynth()\n";
		return 1;
	}	
	
	ret = synths_.erase(synth);
	
	if(pthread_mutex_unlock(&renderMutex_) != 0)
	{
		cerr << "Error: Could not unlock mutex in removeSynth()\n";
		return 1;
	}	
	
	if(ret != 0)
		return 0;
	else
		return 1;	
}

// Clear the render list, removing all synths

void AudioRender::removeAllSynths()
{
	if(pthread_mutex_lock(&renderMutex_) != 0)	// Acquire lock ensuring we're not actively rendering
	{
		cerr << "Error: Could not lock mutex in removeAllSynths()\n";
		return;
	}		
	
	synths_.clear();
	
	if(pthread_mutex_unlock(&renderMutex_) != 0)
	{
		cerr << "Error: Could not unlock mutex in removeAllSynths()\n";
		return;
	}		
}

// This method registers for specific OSC paths when the OscController object is set.  We need a reference to the
// controller so we can unregister on destruction.  Unregistering is handled by the OscHandler destructor.

void AudioRender::setOscController(OscController *c)
{
	if(c == NULL)
		return;
	
	string volumePath("/ui/volume");
	
	OscHandler::setOscController(c);
	
	addOscListener(volumePath);
}

// This method is called by the OscController when it receives a message we've registered for.  In this case
// it's for changing the global volume.  Returns true on success.

bool AudioRender::oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data)
{
	if(strcmp(path, "/ui/volume") || numValues < 1)
		return false;

	float masterVolume, volume = values[0]->f;
	
	if(volume == 0.0)
		masterVolume = 0.0;
	else if(volume >= 0.5)
		masterVolume = pow(4.0, ((double)(volume - 0.5)/0.5));
	else
		masterVolume = pow(10.0, ((double)(volume - 0.5)/0.5));
	
	cout << "Master Volume: " << masterVolume << endl;
	setGlobalAmplitude(masterVolume);
	
	return true;
}

int AudioRender::renderCallback(const void *input, void *output,
								unsigned long frameCount,
								const PaStreamCallbackTimeInfo* timeInfo,
								PaStreamCallbackFlags statusFlags)
{
	// First, initialize the output to all zeros
	bzero(output, frameCount*numOutputChannels_*sizeof(float));
	
	// Walk through the list of synths, calling the render process for each, which will mix its output
	// into the buffer (i.e. not overwrite what's already there).
	// Each synth already knows the sample rate and channel count.
	
	if(pthread_mutex_lock(&renderMutex_) != 0)	// Acquire lock ensuring the render list won't change
	{
		cerr << "Error: Could not lock mutex in renderCallback()\n";
		return paInternalError;
	}
	
	set<SynthBase *>::iterator it;
	for(it = synths_.begin(); it != synths_.end(); it++)
	{
		(*it)->render(input, output, frameCount, timeInfo, statusFlags);
	}
	
	// Scale the output if necessary
	if(globalAmplitude_ != 1.0)
	{
		for(int i = 0; i < frameCount*numOutputChannels_; i++)
			((float *)output)[i] *= globalAmplitude_;
	}
	
	if(pthread_mutex_unlock(&renderMutex_) != 0)	// Release our lock on the render list
	{
		cerr << "Error: Could not unlock mutex in renderCallback()\n";
		return paInternalError;
	}	
	
    return paContinue;
}

AudioRender::~AudioRender()
{
	pthread_mutex_destroy(&renderMutex_);
}
