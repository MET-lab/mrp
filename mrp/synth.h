/*
 *  synth.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/16/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef SYNTH_H
#define SYNTH_H

#include <iostream>
#include <exception>
#include <vector>
#include <list>
#include <deque>
#include <map>
#include <cmath>
#include <pthread.h>
#include "portaudio.h"
#include "parameter.h"
#include "filter.h"
using namespace std;


class SynthBase
{
	friend ostream& operator<<(ostream& output, const SynthBase& s);
public:
	// constructor holds channel info
	SynthBase(float sampleRate);
	
	// render() is called by the PortAudio callback to render an output buffer
	virtual int render(const void *input, void *output,
					   unsigned long frameCount,
					   const PaStreamCallbackTimeInfo* timeInfo,
					   PaStreamCallbackFlags statusFlags) { return 0; }
	
	// These settings are initialized right as the Synth is about to be performed
	void setPerformanceParameters(int numInputChannels, int numOutputChannels, int outputChannel);
	
	// Tell the note to begin
	void begin();
	void begin(PaTime when);
	
	// Tell the note to end, allowing time for any post-release tail
	void release();
	void release(PaTime when);
	
	// Query whether the synth is still running, or whether it has finished its rendering
	bool isRunning() { return isRunning_; }
	bool isFinished() { return isFinished_; }
	
	// Destructor immediately terminates the note.  The note should always be removed from the render
	// list before its destructor is called, or the program is likely to crash.
	
	virtual ~SynthBase();
	
protected:
	/* Stream information */
	int numInputChannels_;
	int numOutputChannels_;
	int outputChannel_;
	float sampleRate_;
	PaTime sampleLength_;	// Time of one sample, inverse of sampleRate
	
	bool isRunning_;		// Whether the note has begun yet
	bool isReleasing_;		// Whether we're in the post-release tail
	bool isFinished_;		// Whether we had started running, then released
	unsigned int sampleNumber_;	// Sample counter between calls, which can wrap as necessary	
	
	PaTime startTime_;		// When this note began
	PaTime releaseTime_;	// When this note should end
};

/*****************
 * class PllSynth
 *
 * Synthesize an output waveform locked in phase to a given input signal.
 * This class includes preprocessing for multiple inputs (delays and gains)
 * to isolate the desired signal (e.g. beam-forming strategies), a bandpass
 * filter to isolate the desired frequency, and a phase-locked loop to synthesize
 * a clean output signal.  The output waveform is continuously variable as a sum of
 * harmonics.
 *****************/

class PllSynth : public SynthBase
{
	friend ostream& operator<<(ostream& output, const PllSynth& s);
public:
	PllSynth(float sampleRate);
	PllSynth(const PllSynth& copy);		// Copy constructor
	
	// Inherited methods from SynthBase
	int render(const void *input, void *output,
					   unsigned long frameCount,
					   const PaStreamCallbackTimeInfo* timeInfo,
						PaStreamCallbackFlags statusFlags);
	
	// These parameters are time-invariant, so we don't use the Parameter structure for them
	void setFilterQ(double filterQ);
	void setLoopFilterPole(float loopFilterPole);
	void setLoopFilterZero(float loopFilterZero);
	void setLoopFilterPoleZero(float loopFilterPole, float loopFilterZero);			// One or the other of these
	void setLoopFilterAB(vector<double>& loopFilterA, vector<double>& loopFilterB); // methods...
	void setUseAmplitudeFeedback(bool useAmplitudeFeedback);
	void setUseInterferenceRejection(bool useInterferenceRejection);
	
	// These methods replace the current parameters with new ones, starting immediately
	void setInputGains(vector<double>& currentInputGains,
					   vector<timedParameter>& rampInputGains);
	void setInputDelays(vector<double>& currentInputDelays,
					    vector<timedParameter>& rampInputDelays);	
	void setCenterFrequency(double currentCenterFrequency, timedParameter& rampCenterFrequency);
	void setLoopGain(double currentLoopGain, timedParameter& rampLoopGain);
	void setPhaseOffset(double currentPhaseOffset, timedParameter& rampPhaseOffset);
	void setGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude);
    vector<double> getCurrentHarmonicAmplitudes();
	void setHarmonicAmplitudes(vector<double>& currentHarmonicAmplitudes,
							   vector<timedParameter>& rampHarmonicAmplitudes);
	void setHarmonicPhases(vector<double>& currentHarmonicPhases,
						   vector<timedParameter>& rampHarmonicPhases);
	void setAmplitudeFeedbackScaler(double currentScaler, timedParameter& rampScaler);
	
	// These methods schedule new parameter additions at the end of the current ones
	void appendInputGains(vector<timedParameter>& inputGains);
	void appendInputDelays(vector<timedParameter>& inputDelays);
	void appendCenterFrequency(timedParameter& centerFrequency);
	void appendLoopGain(timedParameter& loopGain);
	void appendPhaseOffset(timedParameter& phaseOffset);
	void appendGlobalAmplitude(timedParameter& amplitude);
	void appendHarmonicAmplitudes(vector<timedParameter>& harmonicAmplitudes);
	void appendHarmonicPhases(vector<timedParameter>& harmonicPhases);
	void appendAmplitudeFeedbackScaler(timedParameter& scaler);
	
	~PllSynth();
private:
	/* Common Parameters to BPF and PLL */
	Parameter *centerFrequency_;				// Center frequency of the PLL and BPF
	
	bool useAmplitudeFeedback_;				// If true, use feedback on output levels to enforce the desired amplitude
	bool useInterferenceRejection_;			// If true, scales down the loop gain in the presence of interfering
											// signals a semitone above or below
	
	/* Pre-filter input processing */
	vector<Parameter*> inputGains_;			// Gains and delays of several inputs.  For one	
	vector<Parameter*> inputDelays_;		// input, these reduce to 1 and 0, respectively.
	
	/* Bandpass filter */
	float filterQ_;						// Q of the bandpass filter (doesn't change over time)
	float filterQinverse_;				// Inverse of Q, for calculating bandwidth
	
	ButterBandpassFilter *mainInputFilter_;		// Amplitude feedback requires BPFs and envelope followers
	ButterBandpassFilter *lowInputFilter_;		// at any harmonics we want to use.  Interference rejection
	ButterBandpassFilter *highInputFilter_;		// requires BPFs and followers at neighboring frequencies.
	vector<ButterBandpassFilter*> harmonicInputFilters_;	
	
	EnvelopeFollower *mainEnvelopeFollower_;
	EnvelopeFollower *lowEnvelopeFollower_;
	EnvelopeFollower *highEnvelopeFollower_;
	vector<EnvelopeFollower*> harmonicEnvelopeFollowers_;
	
	Parameter *amplitudeFeedbackScaler_;
	
	/* Phase-locked loop */
	GenericFilter *loopFilter_;		
	double loopFilterPole_, loopFilterZero_;
	
	Parameter *loopGain_;					// Overall loop gain (when 0, output = centerFrequency_)	
	Parameter *phaseOffset_;					// Phase offset of output waveform versus internal PLL
	
	/* Output parameters */
	Parameter *globalAmplitude_;			// Overall amplitude
	vector<Parameter*> harmonicAmplitudes_;	// Amplitudes of output harmonics
	vector<Parameter*> harmonicPhases_;		// Phase offset of each harmonic
	
	/* Internal variables */	
	double pllPhase_;			// Current phase of the main PLL, from which all others are derived
		
	bool usingDelayAndSum_;		// Whether we're using multiple inputs, or false for one input with gain 1.0
	bool loopGainWasZero_;		// State information on whether loop gain was zero the last time around
	
	float pllLastOutput_;			// One sample of memory for the PLL loop
	
	pthread_mutex_t parameterMutex_;	// Make sure parameters don't update during ramp
};

/*****************
 * class NoiseSynth
 *
 * Send filtered noise to the output, using any number of bandpass filters of adjustable
 * Q, center frequency, and amplitude.  This class does not make use of an audio input.
 *****************/
class NoiseSynth : public SynthBase
{
	friend ostream& operator<<(ostream& output, const NoiseSynth& s);
public:
	NoiseSynth(float sampleRate);
	NoiseSynth(const NoiseSynth& copy);

	// Inherited methods from SynthBase
	int render(const void *input, void *output,
			   unsigned long frameCount,
			   const PaStreamCallbackTimeInfo* timeInfo,
			   PaStreamCallbackFlags statusFlags);

	// These methods replace the current parameters with new ones, starting immediately
	void setGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude);
	void setFilterFrequencies(vector<double>& currentFrequencies, vector<timedParameter>& rampFrequencies);
	void setFilterQs(vector<double>& currentQs, vector<timedParameter>& rampQs);
	void setFilterAmplitudes(vector<double>& currentAmplitudes, vector<timedParameter>& rampAmplitudes);
	
	// These methods schedule new parameter additions at the end of the current ones
	void appendGlobalAmplitude(timedParameter& amplitude);
	void appendFilterFrequencies(vector<timedParameter>& frequencies);
	void appendFilterQs(vector<timedParameter>& qs);
	void appendFilterAmplitudes(vector<timedParameter>& amplitudes);
	
	~NoiseSynth();
private:
	void updateFilters();				   // Update ButterBandpassFilter objects after set/append
	
	Parameter *globalAmplitude_;		   // Overall amplitude
	vector<Parameter*> filterFrequencies_; // Frequency, Q, and level for an arbitrary number of BPFs
	vector<Parameter*> filterQs_;		   // If no filters are defined, output is white noise with
	vector<Parameter*> filterAmplitudes_;  // amplitude globalAmplitude
	
	vector<ButterBandpassFilter*> filters_;	// This holds the actual filter objects
	
	pthread_mutex_t parameterMutex_;	// Make sure parameters don't update during ramp	
};

/*****************
 * class PlaythroughSynth
 *
 * Send a combination of input channels directly to the output channel.  The inputs are each
 * adjustable by amplitude and delay.
 *****************/
class PlaythroughSynth : public SynthBase
{
public:
	void setAmplitudes(vector<timedParameter> amplitudes);
	void setDelays(vector<timedParameter> delays);
	
	void appendAmplitudes(vector<timedParameter> amplitudes);
	void appendDelays(vector<timedParameter> delys);
	
private:
	vector<timedParameter> inputGains_;	
	vector<double> currentInputGains_;	
	vector<timedParameter> inputDelays_;
	vector<double> currentInputDelays_;		
};

/*****************
 * class ResonanceSynth
 *
 * Reinforces the natural overtones of a given string, depending on input from other MIDI notes.
 *****************/
class ResonanceSynth : public SynthBase
{
public:
	ResonanceSynth(float sampleRate);
	ResonanceSynth(const ResonanceSynth& copy);	
	
	// Inherited methods from SynthBase
	int render(const void *input, void *output,
			   unsigned long frameCount,
			   const PaStreamCallbackTimeInfo* timeInfo,
			   PaStreamCallbackFlags statusFlags);
	
	// These methods replace the current parameters with new ones, starting immediately
	void setGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude);
	void setHarmonicRolloff(double currentRolloff, timedParameter& rampRolloff);
	void setDecayRate(double currentRate, timedParameter& rampRate);
	void setMono(bool mono);
	
	// These methods schedule new parameter additions at the end of the current ones
	void appendGlobalAmplitude(timedParameter& amplitude);
	void appendHarmonicRolloff(timedParameter& rolloff);
	void appendDecayRate(timedParameter& rate);
	
	// This method is called by the note method to add a new decaying sinusoid
	void addHarmonic(int midiNoteKey, float frequency, float amplitude);
	
	~ResonanceSynth();
private:
	// Parameters
	Parameter *globalAmplitude_;			// Total strength of synthesized notes
	Parameter *harmonicRolloff_;			// How much to de-emphasize higher partials
	Parameter *decayRate_;					// How quickly the harmonics decay (normalized to middle C)
	bool mono_;								// If true, use only one harmonic at a time
	
	// State variables
	typedef struct {
		float frequency;					// Hold information on currently sounding harmonics					
		float phase;
		EnvelopeFollower *amplitude;
	} harmonicInfo;
	
	map<unsigned int, harmonicInfo> harmonics_;	// Which harmonics are currently sounding
	
	pthread_mutex_t parameterMutex_;	// Make sure parameters don't update during ramp		
	pthread_mutex_t noteMutex_;
};

#endif // SYNTH_H
