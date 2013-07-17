/*
 *  pitchtrack.h
 *  mrp
 *
 *  Created by Andrew McPherson on 11/14/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef PITCHTRACK_H
#define PITCHTRACK_H

#include <iostream>
#include "portaudio.h"
#include "lo/lo.h"
#include "config.h"
#include "audiorender.h"
#include "midicontroller.h"
#include "note.h"
using namespace std;

// This class handles all the central dispatching related to tracking an incoming pitch.  Note that
// the actual tracking takes place externally, with the messages arriving via OSC.  The functions performed
// by this class include:
//   - Parsing pieces of the XML patch table related to pitch-tracking
//   - Allocating new notes and releasing old ones
//   - Routing incoming pitch and amplitude messages to the appropriate notes

#define PITCHTRACK_BUFFER_SIZE	128

class PitchTrackNote;
class PitchTrackSynth;

class PitchTrackController : public OscHandler
{
	friend class MidiController;

public:	
	PitchTrackController(MidiController *midiController);
	
	void parseGlobalSettings(TiXmlElement *baseElement);
	void parsePatchTable(TiXmlElement *pitchTrackElement, int programId);
	
	void allNotesOff();
	void noteEnded(PitchTrackNote *note, unsigned int key);
	void programChanged();
	
	void setInputMute(bool mute) { inputMuted_ = mute; }
	

	bool oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **argv, void *data);
	void setOscController(OscController *c);
	
private:
	// Private functions
	bool handleOscPitch(const char *path, const char *types, int numValues, lo_arg **argv, void *data);
	bool handleOscMute(const char *path, const char *types, int numValues, lo_arg **argv, void *data);
	
	int triggerNote(PitchTrackNote *noteRef, int midiNoteId, int inputMidiNote, int priority);
	vector<int> parseCommaSeparatedValues(const string& inString);
	
	
	MidiController *midiController_;					// Reference to main note controller object
	
	// History: circular buffers
	float frequencyBuffer_[PITCHTRACK_BUFFER_SIZE];		// These hold the last N values of frequency and amplitude
	float amplitudeBuffer_[PITCHTRACK_BUFFER_SIZE];		// History is important when triggering notes
	float fractionalMidiNoteBuffer_[PITCHTRACK_BUFFER_SIZE];
	int pitchSampleCount_[128];							// How many samples have been within tolerance of each pitch
	int frequencyBufferIndex_, amplitudeBufferIndex_, fractionalMidiNoteBufferIndex_;
	
	// Triggering parameters
	int triggerPositiveSamples_, triggerTotalSamples_;	// If X out of Y samples match a pitch, trigger that note
	float pitchToleranceSemitones_;						// What fraction of a semitone a pitch can different and
														// still be considered a "match"
	float amplitudeThreshold_;							// Minimum amplitude for a sample to count as positive
	bool allowOctaveErrors_;							// Do notes off by +/- octave count toward the total?
	bool inputMuted_;									// Whether the input from OSC is muted
	
	// Program and current note info
	
	typedef struct {
		PitchTrackNote *note;							// Note object that does the synthesis
		bool onceOnly;									// Whether this note can be triggered more than once
		int priority;									// Higher # = higher priority, if we run out of channels
		vector<int> coupledNotes;						// Relative MIDI note # of other pitches to trigger
	} PitchTrackProgramInfo;
	
	map<unsigned int, PitchTrackProgramInfo> programs_;	// Info on the current programs
	map<unsigned int, PitchTrackNote *> pitchTrackCurrentNotes_;	
	map<unsigned int, vector<int> > notesToTurnOn_;			// Which notes to enable immediately on program change
	map<unsigned int, vector<int> > notesToTurnOff_;		// Which notes to disable when a particular program loads
	int soundingNotePriorities_[128];					// Priorities of currently sounding notes
	PaTime soundingNoteStartTimes_[128];				// When each note started
	bool notesTriggered_[128];							// Whether each note has been triggered before (for one-shot notes)

#ifdef DEBUG_OSC
	int oscCounter_;
#endif
	
	
	pthread_mutex_t listenerMutex_;						
};

class PitchTrackNote : public MidiNote
{
public:
	PitchTrackNote(MidiController *controller, AudioRender *render, PitchTrackController *ptController);
	
	int parseXml(TiXmlElement *baseElement);		// Get all our settings from an XML file	
	
	// Update this note with new pitch-tracking values; we may want to change synth parameters here
	void pitchTrackValues(float frequency, float amplitude); 
	
	// Subclass this to talk to PitchClassController rather than MidiController
	void abort();	
	
	// This method creates and returns a copy of the object, but instead of containing factories, it contains real
	// Synth objects with the right parameters for the particular MIDI note and velocity.
	
	PitchTrackNote* createNote(int audioChannel, int mrpChannel, int midiNote, int inputMidiNote, int pianoString, unsigned int key,
							   int priority, int velocity, float phaseOffset, float amplitudeOffset);
	
private:
	// Private methods
	void assignPitchTrackSynthParameters(PitchTrackSynth *synth, 
										paramHolder *relativeInputFreq,
										paramHolder *relativeOutputFreq,
										 TiXmlElement *element);
	
	
	// Note parameters
	vector<paramHolder> relativeInputFrequencies_, relativeOutputFrequencies_;
	
	// State variables
	PitchTrackController *pitchTrackController_;
};

// This class provides a simple sinusoid-generating synth (no loop gain) whose frequency and amplitude can
// be adjusted in real-time to an external input.  Some of this code is borrowed from PllSynth.

class PitchTrackSynth : public SynthBase
{
	friend ostream& operator<<(ostream& output, const PllSynth& s);
public:
	PitchTrackSynth(float sampleRate);
	PitchTrackSynth(const PitchTrackSynth& copy);		// Copy constructor
	
	void begin();
	void begin(PaTime when);
	
	// These parameters are time-invariant, so we don't use the Parameter structure for them
	void setMaxDuration(double maxDuration);
	void setDecayTimeConstant(double decayTimeConstant);
	void setHarmonicCentroidMultiply(bool multiply);
	
	// These methods replace the current parameters with new ones, starting immediately
	void setInputCenterFrequency(double currentCenterFrequency, timedParameter& rampCenterFrequency);
	void setOutputCenterFrequency(double currentCenterFrequency, timedParameter& rampCenterFrequency);
	void setMaxGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude);	
	void setInputGain(double currentGain, timedParameter& rampGain);
	void setPitchFollowRange(double currentRange, timedParameter& rampRange);
	void setPitchFollowRatio(double currentRatio, timedParameter& rampRatio);
	void setHarmonicCentroid(double currentCentroid, timedParameter& rampCentroid);	
	void setHarmonicAmplitudes(vector<double>& currentHarmonicAmplitudes,
							   vector<timedParameter>& rampHarmonicAmplitudes);
	void setHarmonicPhases(vector<double>& currentHarmonicPhases,
						   vector<timedParameter>& rampHarmonicPhases);
	
	// These methods schedule new parameter additions at the end of the current ones
	void appendInputCenterFrequency(timedParameter& centerFrequency);
	void appendOuputCenterFrequency(timedParameter& centerFrequency);
	void appendMaxGlobalAmplitude(timedParameter& amplitude);	
	void appendInputGain(timedParameter& inputGain);	
	void appendPitchFollowRange(timedParameter& range);
	void appendPitchFollowRatio(timedParameter& ratio);	
	void appendHarmonicCentroid(timedParameter& inputCentroid);	
	void appendHarmonicAmplitudes(vector<timedParameter>& harmonicAmplitudes);
	void appendHarmonicPhases(vector<timedParameter>& harmonicPhases);	
	
	void setFrequencyAmplitudeData(double freq, double amp);
	
	// Inherited methods from SynthBase
	int render(const void *input, void *output,
			   unsigned long frameCount,
			   const PaStreamCallbackTimeInfo* timeInfo,
			   PaStreamCallbackFlags statusFlags);	
	
	~PitchTrackSynth();
private:
	// Time-invariant parameters
	double maxDuration_;
	double decayTimeConstant_;
	bool harmonicCentroidMultiply_;
	
	// Time-variant parameters
	Parameter *inputCenterFrequency_;		// Center frequency of the input note to listen to
	Parameter *outputCenterFrequency_;		// Center frequency of the synthesized note
	Parameter *pitchFollowRange_;			// How far (in frequency ratio) to deviate from supposed center pitch
	Parameter *pitchFollowRatio_;			// How closely to track the incoming pitch
	Parameter *maxGlobalAmplitude_;			// Overall amplitude
	Parameter *inputGain_;					// How much the input amplitude is scaled by (but never exceeding maxGlobalAmplitude)	
	Parameter *harmonicCentroid_;			// Adjustment to previously set harmonics (add or multiply in frequency)
	
	vector<Parameter*> harmonicAmplitudes_;	// Amplitudes of output harmonics
	vector<Parameter*> harmonicPhases_;		// Phase offset of each harmonic	
	
	// Other state
	EnvelopeFollower *inputEnvelopeFollower_;	// Follows the (scaled) input amplitude
	
	double phase_;			// Current phase of the main oscillator, from which all others are derived
	double lastFrequency_, lastAmplitude_;		// Last input values from pitch tracker
	double minInputFrequency_, maxInputFrequency_;	// The range of input frequencies that are "in range"
	bool shouldRelease_;							// Works in conjunction with maxDuration_
	
	pthread_mutex_t parameterMutex_;	// Make sure parameters don't update during ramp	
	pthread_mutex_t freqAmpMutex_;		// Mutex to handle more frequent frequency/amplitude updates
};

#endif // PITCHTRACK_H