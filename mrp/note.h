/*
 *  note.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/29/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef NOTE_H
#define NOTE_H

#include <iostream>
#include <utility>
#include "tinyxml.h"
#include "synth.h"
#include "audiorender.h"
#include "midicontroller.h"
#include "osccontroller.h"

using namespace std;

class MidiController;

// This class implements a single note, which may control multiple synth objects.  Notes are triggered
// by the MIDI controller, which will also pass additional messages (note off, control change, etc.).
class Note
{
    friend class MidiController;
public:
	// Constructor receives a reference to the MIDI Controller for various callback purposes
	Note(MidiController *controller, AudioRender *render) {	
		controller_ = controller;
		render_ = render;
		isRunning_ = false;
		audioChannel_ = mrpChannel_ = 0;
		startTime_ = (double)render_->currentTime();
#ifdef DEBUG_ALLOCATION
		cout << "*** Note\n";
#endif
	}
	//Note(Note& copy);
	
	// Initializer methods:
	
	virtual int parseXml(TiXmlElement *baseElement) { return -1; }	// Parse an XML file containing all relevant note settings
																// Returns 0 on success.  Each type of Note defines its own settings
	void setPerformanceParameters(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, 
								  unsigned int key, int priority) {
		audioChannel_ = audioChannel;						// Set the parameters that differ for each performance of this note
		mrpChannel_ = mrpChannel;
		midiNote_ = midiNote;
		midiChannel_ = midiChannel;
		pianoString_ = pianoString;
		key_ = key;		
		priority_ = priority;
	}
	
	virtual void setResponseToPedals(bool damper, bool sostenuto) {} // Tells this note whether to continue playing when sustained
																	 // by damper or sostenuto pedals (but not by the key). 
	// Accessor methods:
	
	int audioChannel() { return audioChannel_; }
	int mrpChannel() { return mrpChannel_; }
	int midiNote() { return midiNote_; }
	int midiChannel() { return midiChannel_; }
	int pianoString() { return pianoString_; }
	double startTime() { return startTime_; }
	int priority() { return priority_; }
	
	// Performance control methods:
	//   begin(): start the note now; usually triggered by MIDI Note On, and immediately follows the constructor
	//   release(): usually called from MIDI Note Off.  Sound might stop, or there might be some kind of tail on this note
	//   releaseDamperDown(): a stronger form of release called when the damper on the string comes down.  
	//				   In most cases the note should stop right away.
	//   abort(): stop the sound immediately, no questions asked
	
	virtual void begin(bool damperLifted) { isRunning_ = true; }								
	virtual void release() { abort(); }						
	virtual void releaseDamperDown() { abort(); }					
	virtual void abort() { 
		if(isRunning_)
			controller_->noteEnded(this, key_);
		isRunning_ = false;
	}								
	
	// This method is called from an external thread to check whether the note is finished.  Should return true if all
	// synths are done.  Each Note subclass should override this to provide its own implementation, otherwise the note
	// will be terminated immediately.
	
	virtual bool isFinished() { return true; }	
	
	// MIDI data methods: these are called whenever relevant MIDI data comes in.  The Note object can call a filter method
	// in MidiController to set which messages it wants to receive.  By default, these methods do nothing
	
	// This is assumed to either be for our channel, or for all channels
	virtual void midiAftertouch(unsigned char value) {}
	virtual void midiControlChange(unsigned char channel, unsigned char control, unsigned char value) {}		
	virtual void midiPitchWheelChange(unsigned int value) {}
	virtual void midiNoteEvent(unsigned char status, unsigned char note, unsigned char velocity) {}
	
	// TODO: OSC messages
	
	virtual ~Note() { 
		abort(); 
#ifdef DEBUG_ALLOCATION
		cout << "*** ~Note\n"; 
#endif
	}

protected:
	MidiController *controller_;			// Reference to the MIDI controller object, for callbacks etc
	AudioRender *render_;					// Reference to the AudioRender object which actually handles the rendering
	int audioChannel_;						// Which DAC channel we use.  Guaranteed to be valid.
	int mrpChannel_;						// Which input of the MRP controller this uses; might be different than audioChannel
	int midiNote_;							// What MIDI note triggered this note (different, potentially, from
											// both the piano string and the actual pitch we play)
	int midiChannel_;						// Which MIDI channel this note triggers from (-1 for non-MIDI).
	int pianoString_;						// Which piano string (by MIDI note #) this note routes to.
											// Note that this might be different than the MIDI pitch we play
	double startTime_;						// When this note started
	int priority_;							// Higher numbers mean higher priority (for purposes of turning off when out of channels)
	unsigned int key_;						// Internal variable to be used to find this note in MidiController's map
	bool isRunning_;						// Whether this note is running or not
};

class MidiNote : public Note
{
protected:
	class SynthBaseFactory;
	class PllSynthFactory;
	class NoiseSynthFactory;
	
public:
	MidiNote(MidiController *controller, AudioRender *render) : Note(controller, render) { 
		isReleasing_ = false;
		velocityCurve_ = 0.0;
#ifdef DEBUG_ALLOCATION
		cout << "*** MidiNote\n"; 
#endif
	}
	//MidiNote(const MidiNote &copy);					// Copy constructor
	//MidiNote& operator=(const MidiNote& copy);
	
	int parseXml(TiXmlElement *baseElement);		// Get all our settings from an XML file

	void setPerformanceParameters(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key,
								  int priority, int velocity);
	void setResponseToPedals(bool damper, bool sostenuto);
	
	void begin(bool damperLifted);
	void release();									// subclass this to allow pedal checking
	void abort();									// releaseDamperDown() also calls abort()
	
	bool isFinished();								// Whether all our synths are finished...
	
	// begin: inserts the synths into the audiorender queue
	// release, releaseDamperDown, abort: tells all synths to release now, and removes them from the queue
	//   eventually, notes with a tail will have a different release and releaseDamperDown method than abort()
	// cleanupIfFinished: check if all synths are done; if so, remove them from the queue and ourselves from the controller
	
	void midiControlChange(unsigned char channel, unsigned char control, unsigned char value);

	// This method creates and returns a copy of the object, but instead of containing factories, it contains real
	// Synth objects with the right parameters for the particular MIDI note and velocity.
	
	MidiNote* createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key,
						 int priority, int velocity, float phaseOffset, float amplitudeOffset);
	
	~MidiNote();

	// Text parsers -- eventually move this to a separate class?
	static timedParameter parseParameterRamp(TiXmlElement *element);
	static void parseParameterRampWithVelocity(TiXmlElement *element, timedParameter *out1, timedParameter *out2);
	static vector<timedParameter> parseMultiParameterRamp(TiXmlElement *element);
	static void parseMultiParameterRampWithVelocity(TiXmlElement *element, vector<timedParameter> *out1, vector<timedParameter> *out2);
	static vector<double> parseCommaSeparatedValues(const string& inString);
	static void parseCommaSeparatedValuesWithVelocity(const char *inString, vector<double> *out1, vector<double> *out2);
	static int parseVelocityPair(const char *inString, double *outA, double *outB);
	static double strtod_with_suffix( const char * str );

protected:
	// ***** Private Methods ******
	
	void assignPllSynthParameters(PllSynthFactory *factory, TiXmlElement *element);
	void assignNoiseSynthParameters(NoiseSynthFactory *factory, TiXmlElement *element);
	
	
	// ***** Private Variables ******
	vector<SynthBase*> synths_;						// These are the Synth objects that will do the audio rendering.
													// This type of note can have one or more of them.
	vector<SynthBaseFactory*> factories_;			// These are special containers that hold a range of possible
													// parameter values for each synth.  When createNote() is called a new
													// MidiNote is created which holds synths instead of factories.

	int velocity_;									// MIDI velocity number
	string name_;									// The name of this patch
	bool sustainOnDamperPedal_, sustainOnSostenutoPedal_;	// Whether or not this note continues if prolonged by this pedal
	float velocityCurve_;							// Curvature of velocity parameter, for transeg (0 = linear)
	bool isReleasing_;								// Whether we're in releasing mode (i.e. waiting for a pedal change)
	
	// ***** Factory Classes ******
	// The idea here is to store the range of possible parameters a synth might take, since we won't know the
	// exact set of values until we know the MIDI note number and velocity coming in.  These classes contain a method
	// to instantiate a new Synth with the correct values.
	
	typedef struct {		// This holds the starting and ramping values of each parameter
		double start;					// Saves using separate variables to hold them.  In synth.h, we use actual Parameter
		timedParameter ramp;			// objects, but we don't want all that baggage here.
	} paramHolder;
	
	class SynthBaseFactory	// Make all members public, but only MidiNote can see them since the class is private
	{
	public:
		SynthBaseFactory(MidiController *controller) { 
			controller_ = controller; 
#ifdef DEBUG_ALLOCATION
			cout << "*** SynthBaseFactory\n"; 
#endif
		}
		float sampleRate_;	
		
		// This creates a new object and returns it.  The object will eventually need to be deleted elsewhere.
		virtual SynthBase* createSynth(int note, int velocity, float velocityCurvature, 
									   float amplitudeOffset) { return new SynthBase(sampleRate_); }
		virtual ~SynthBaseFactory() { 
#ifdef DEBUG_ALLOCATION
			cout << "*** ~SynthBaseFactory\n";
#endif
		}
	protected:
		// This function calculates a user-defined curve similar to csound's transeg opcode
		double transeg(double val1, double val2, double concavity, double velocity);
		
		MidiController *controller_;
	};
	
	class PllSynthFactory : public SynthBaseFactory
	{
	public:								
		PllSynthFactory(MidiController *controller) : SynthBaseFactory(controller), useAmplitudeFeedbackActive_(false),
		  useInterferenceRejectionActive_(false), filterQActive_(false), loopFilterPoleActive_(false), loopFilterZeroActive_(false),
		  relativeFrequencyActive_(false), globalAmplitudeActive_(false), loopGainActive_(false), amplitudeFeedbackScalerActive_(false),
		  inputGainsActive_(false), inputDelaysActive_(false), harmonicAmplitudesActive_(false), harmonicPhasesActive_(false) {}
		
		// Non-ramping (non-velocity-sensitive) parameters
		bool useAmplitudeFeedback_, useInterferenceRejection_;
		float filterQ_, loopFilterPole_, loopFilterZero_;
		bool useAmplitudeFeedbackActive_, useInterferenceRejectionActive_, filterQActive_, loopFilterPoleActive_, loopFilterZeroActive_;
		
		// Single ramping parameters
		paramHolder relativeFrequencyMin_, relativeFrequencyMax_;	// Substitutes for centerFrequency until we know MIDI note
		paramHolder globalAmplitudeMin_, globalAmplitudeMax_;
		paramHolder loopGainMin_, loopGainMax_;
		paramHolder amplitudeFeedbackScalerMin_, amplitudeFeedbackScalerMax_;
		bool relativeFrequencyActive_, globalAmplitudeActive_, loopGainActive_, amplitudeFeedbackScalerActive_;
		float relativeFrequencyConcavity_, globalAmplitudeConcavity_, loopGainConcavity_, amplitudeFeedbackScalerConcavity_;
		
		// Vector ramping parameters
		vector<paramHolder> inputGainsMin_, inputGainsMax_;
		vector<paramHolder> inputDelaysMin_, inputDelaysMax_;
		vector<paramHolder> harmonicAmplitudesMin_, harmonicAmplitudesMax_;
		vector<paramHolder> harmonicPhasesMin_, harmonicPhasesMax_;
		bool inputGainsActive_, inputDelaysActive_, harmonicAmplitudesActive_, harmonicPhasesActive_;
		float inputGainsConcavity_, inputDelaysConcavity_, harmonicAmplitudesConcavity_, harmonicPhasesConcavity_;
		
		PllSynth* createSynth(int note, int velocity, float velocityCurvature, float amplitudeOffset);
	};
	
	class NoiseSynthFactory : public SynthBaseFactory
	{
	public:
		NoiseSynthFactory(MidiController *controller) : SynthBaseFactory(controller), globalAmplitudeActive_(false),
		  filterFrequenciesActive_(false), filterQsActive_(false), filterAmplitudesActive_(false) {}
		
		// Single ramping parameters
		paramHolder globalAmplitudeMin_, globalAmplitudeMax_;
		bool globalAmplitudeActive_;
		float globalAmplitudeConcavity_;
		
		// Vector ramping parameters
		vector<paramHolder> filterFrequenciesMin_, filterFrequenciesMax_;
		vector<paramHolder> filterQsMin_, filterQsMax_;
		vector<paramHolder> filterAmplitudesMin_, filterAmplitudesMax_;
		bool filterFrequenciesActive_, filterQsActive_, filterAmplitudesActive_;
		float filterFrequenciesConcavity_, filterQsConcavity_, filterAmplitudesConcavity_;
	
		NoiseSynth* createSynth(int note, int velocity, float velocityCurvature, float amplitudeOffset);
	};
};


#pragma mark class CalibratorNote

// This class implements a calibration note.  The differences from MidiNote are:
//   (1) It is monophonic: a new note coming in of this type will stop the old one
//   (2) It listens to a pair of MIDI controllers to change the phase offset and amplitude of the current note
//   (3) It can write these values to a special XML file that is loaded on startup
//   (4) It does not need any Synth parameters; one internal synth is loaded automatically

class CalibratorNote : public MidiNote, public OscHandler
{
public:
	CalibratorNote(MidiController *controller, AudioRender *render);
	
	int parseXml(TiXmlElement *baseElement);		// Override the MidiNote XML parser

	void begin(bool damperLifted);					// Override to enable control change messages
	void midiControlChange(unsigned char channel, unsigned char control, unsigned char value);
	void calibrateSetPhase(float phaseOffset);
	void calibrateSetAmplitude(float amplitudeOffset);
	
	// This method creates and returns a copy of the object, but instead of containing factories, it contains real
	// Synth objects with the right parameters for the particular MIDI note and velocity.
	
	CalibratorNote* createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key,
							   int priority, int velocity, float phaseOffset, float amplitudeOffset);
	
	// OSC handler routine, for changing calibration settings
	bool oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data);
	void setOscController(OscController *c);	// Override the OscHandler implementation to register our paths
	
	~CalibratorNote();
private:	
	// ***** Private Variables ******

	double calibratorGlobalAmplitude_;
	unsigned int phaseControl_, amplitudeControl_;		// Which controllers we listen to for adjusting phase and amplitude offset
	unsigned int phaseControlChannel_, amplitudeControlChannel_;	// Channels for the above controllers

	// TODO: Auto-calibrator synth (but this will need to be better tested... do this later)
};

#pragma mark class ResonanceNote

// This class implements a note that enhances the natural sympathetic vibrations on the piano.  When
// triggered for a particular string (usually in the bass register), it listens to subsequent note events
// and synthesizes overtones of the low string.

class ResonanceNote : public MidiNote
{
public:
	ResonanceNote(MidiController *controller, AudioRender *render);
	
	int parseXml(TiXmlElement *baseElement);
	void begin(bool damperLifted);
	void midiNoteEvent(unsigned char status, unsigned char note, unsigned char velocity);
	
	// This method creates and returns a copy of the object, but instead of containing factories, it contains real
	// Synth objects with the right parameters for the particular MIDI note and velocity.
	
	ResonanceNote* createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key,
							  int priority, int velocity, float phaseOffset, float amplitudeOffset);	
	
	~ResonanceNote();
	
private:
	void assignResonanceSynthParameters(ResonanceSynth *synth, TiXmlElement *element);
	
	set<int> harmonicallyRelated_;
};

#endif // NOTE_H