/*
 *  realtimenote.h
 *  mrp
 *
 *  Created by Andrew McPherson on 2/14/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef REALTIMENOTE_H
#define REALTIMENOTE_H

#include <iostream>
#include <vector>
#include "note.h"
#include "midicontroller.h"
using namespace std;

#define DEBUG_ALLOCATION
#define DEBUG_MESSAGES_EXTRA

#define MAX_CENTROID_HARMONICS	16


class RealTimeMidiNote : public MidiNote
{
    friend class MidiController;
    
protected:
	class PllSynthQuality;
	
public:
	RealTimeMidiNote(MidiController *controller, AudioRender *render) : MidiNote(controller, render) {
#ifdef DEBUG_ALLOCATION
		cout << "*** RealTimeMidiNote\n"; 
#endif
		intensity_ = new PllSynthQuality;
		brightness_ = new PllSynthQuality;
		pitch_ = new PllSynthQuality;
		harmonic_ = new PllSynthQuality;
		keyDownHoldoffTime_ = 0.0;
		keyDownHoldoffScaler_ = 1.0;
		useKeyDownHoldoff_ = false;
		harmonicSweepRange_ = 0;
		harmonicSweepSpread_ = 0;
		usingRawHarmonics_ = false;
		usePitchBendWithHarmonics_ = false;
	}
	
	// Override certain MidiNote methods:
	
	int parseXml(TiXmlElement *baseElement);	// Get a few extra settings here
	
	// Set the starting parameters (before real-time modulation) for this note.  Do this before begin().	
	RealTimeMidiNote* createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, 
								 unsigned int key, int priority, int velocity, float phaseOffset, float amplitudeOffset,
								 double attack, double intensity, double brightness, double pitchBase);
	
	// Continuous parameter modulation
	
	// A thought: can we make generic quality-to-parameter maps that look up in some sort of table?  Behavior could
	// be defined by XML and then we just turn the knob to slide along a scale.
	
	double getIntensityBase() {						// Intensity is a map to amplitude and harmonic content
		return intensity_->currentBaseValue();
	}
	void setAbsoluteIntensityBase(double absoluteVal) {	// reduced to a linear scale
		intensity_->setBaseValue(absoluteVal);
	}
	void setRelativeIntensityBase(double relativeVal) {
		intensity_->setBaseValue(intensity_->currentBaseValue() + relativeVal);
	}
	
	double getBrightness() {							// Brightness is an independent map to harmonic content,
		return brightness_->currentBaseValue();
	}
	void setAbsoluteBrightness(double absoluteVal) {	//  relative to the current intensity
		brightness_->setBaseValue(absoluteVal);
	}
	void setRelativeBrightness(double relativeVal) {
		brightness_->setBaseValue(brightness_->currentBaseValue() + relativeVal);
	}
	
	double getPitchBase() {							// Frequency base is relative to the fundamental frequency
		return pitch_->currentBaseValue();
	}
	void setAbsolutePitchBase(double absoluteVal) {	// of the MIDI note
		pitch_->setBaseValue(absoluteVal);
	}
	void setRelativePitchBase(double relativeVal) {
		pitch_->setBaseValue(pitch_->currentBaseValue() + relativeVal);
	}
	//static double semitoneRatio(double numSemitones) {	// helper method to convert # of semitones to a freq ratio
	//	return pow(2.0, numSemitones/12.0);
	//}
	double getHarmonicBase() {						
		return harmonic_->currentBaseValue();
	}
	void setAbsoluteHarmonicBase(double absoluteVal) {	
		harmonic_->setBaseValue(absoluteVal);
	}
	void setRelativeHarmonicBase(double relativeVal) {
		harmonic_->setBaseValue(harmonic_->currentBaseValue() + relativeVal);
	}	
	
	double getIntensityVibrato() {							// Intensity vibrato (aka tremolo) is a periodic modulation
		return intensity_->currentVibratoValue();
	}
	void setAbsoluteIntensityVibrato(double absoluteVal) {	// in the intensity parameter (above).  These methods take
		intensity_->setVibratoValue(absoluteVal);
	}
	void setRelativeIntensityVibrato(double relativeVal) {	// values centered around zero to modulate the current level
		intensity_->setVibratoValue(intensity_->currentVibratoValue() + relativeVal);
	}
	double getPitchVibrato() {							// Frequency vibrato is a periodic modulation in frequency
		return pitch_->currentVibratoValue();
	}
	void setAbsolutePitchVibrato(double absoluteVal) {	// Similarly zero-centered (+/-1 maps to range loaded from XML)
		pitch_->setVibratoValue(absoluteVal);
	}
	void setRelativePitchVibrato(double relativeVal) {
		pitch_->setVibratoValue(pitch_->currentVibratoValue() + relativeVal);
	}
	double getHarmonicVibrato() {							// Frequency vibrato is a periodic modulation in frequency
		return harmonic_->currentVibratoValue();
	}
	void setAbsoluteHarmonicVibrato(double absoluteVal) {	// Similarly zero-centered (+/-1 maps to range loaded from XML)
		harmonic_->setVibratoValue(absoluteVal);
	}
	void setRelativeHarmonicVibrato(double relativeVal) {
		harmonic_->setVibratoValue(harmonic_->currentVibratoValue() + relativeVal);
	}
	/*float getPitchBend() {							// Similar to frequency vibrato but with a possibly different
		return pitchBend_.currentValue();
	}
	void setAbsolutePitchBend(float absoluteVal);	// range and designed for slower control
	void setRelativePitchBend(float relativeVal);	*/
	
	// Since there is a great deal of overlap among the above parameters, we need some methods to get the
	// combined qualities of all of them.
	
	double getCurrentIntensity() {					// Both of these sum the effects of base and vibrato/bend
		return intensity_->currentCombinedValue();
	}
	double getCurrentPitch() {
		return pitch_->currentCombinedValue();
	}
	double getCurrentHarmonic() {
		return harmonic_->currentCombinedValue();
	}
	
	double keyDownHoldoffTime() { return keyDownHoldoffTime_; }			// Parameters on note behavior after
	double keyDownHoldoffScaler() { return keyDownHoldoffScaler_; }		// hammer strike
	bool useKeyDownHoldoff() { return useKeyDownHoldoff_; }
	
	int harmonicSweepRange() { return harmonicSweepRange_; }			// Whether we redefine keys above this one to change harmonics
	int harmonicSweepSpread() { return harmonicSweepSpread_; }			// Adjusts difference in harmonics for multiple simultaneous notes
	bool usePitchBendWithHarmonics() { return usePitchBendWithHarmonics_; }
    vector<double> getCurrentHarmonicAmplitudes();
	void setRawHarmonicValues(vector<double>& values);					// Set the raw values directly
	void clearRawHarmonicValues();										// Clear out the previous raw setting
	
	// One-shot parameters which can be set or re-set in real-time
	
	void setAttack(double attack);
	//void setReleaseProfile(double decayRate, double vibratoDepth, double vibratoRate, double vibratoPhase);	// Later!
	
	void updateSynthParameters();		// Call this when all qualities have been updated to calculate a new
										// collection of synth parameters.  Usually this will be done once per
										// PianoBar buffer.
    
    /*! This method encapsulates the setting of absolute pitch by determining the correct scaling of relative pitch (inverse voodoo).
        If scaleIntensity == true, we boost the intensity of pitches proportional to their distance from the string's fundamental */
    void setAbsolutePitch(double midiNotePitch, double targetPitch, bool scaleIntensity);
    
    ~RealTimeMidiNote();

	
protected:
	// ******* Mappings from quality to synth parameters *******
	
#pragma mark private class PllSynthQuality	
	class PllSynthQuality
	{
	public:
		PllSynthQuality() : useGlobalAmplitude_(false), useRelativeFrequency_(false), useHarmonicAmplitudes_(false),
			useHarmonicPhases_(false), useLoopGain_(false), useHarmonicCentroid_(false), currentBaseValue_(0.0), 
			currentVibratoValue_(0.0), vibratoWeight_(0.0) {}
		int parseXml(TiXmlElement *baseElement);	// Get specific settings from an XML file
		
		double currentBaseValue() { return currentBaseValue_; }	// Return the current value of this quality
		void setBaseValue(double value);								// Set the current quality value
		
		double currentVibratoValue() { return currentVibratoValue_; }
		void setVibratoValue(double value);
		
		double currentCombinedValue() {							// Sum the effects of base and vibrato
			//cout << "base " << currentBaseValue_ << " vib " << currentVibratoValue_ << endl;
			if(vibratoWeight_ == 0)
				return currentBaseValue_;
			else
			{
				double val = currentBaseValue_ + vibratoWeight_*currentVibratoValue_;
				
				if(val < 0 && clipVibratoLower_)
					return 0;
				if(val > 1 && clipVibratoUpper_)
					return 1;
				return val;
			}
		}

		// These methods take in a current parameter value and update it as necessary with its current
		// values.  If this quality doesn't affect a particular parameter (e.g. useGlobalAmplitude_ = false),
		// returns the same value it gets in.
		
		double scaleGlobalAmplitude(double inGlobalAmplitude, bool *updated = NULL);
		double scaleRelativeFrequency(double inRelativeFrequency, bool *updated = NULL);
		vector<double> scaleHarmonicAmplitudes(vector<double>& inHarmonicAmplitudes, bool *updated = NULL);
		vector<double> scaleHarmonicPhases(vector<double>& inHarmonicPhases, bool *updated = NULL);
		double scaleLoopGain(double inLoopGain, bool *updated = NULL);
        
        vector<double> getRelativeFrequencyRange()
        {
            vector<double> min_max;
            min_max.push_back(relativeFrequencyMin_);
            min_max.push_back(relativeFrequencyMax_);
            return min_max;
        }
        
        double getCurrentRelativeFrequency()
        {
            return currentRelativeFrequency_;
        }
                             
        void resetHarmonicAmplitudes(vector<double> &targetHarmonicAmplitudes);
		
	private:
		void updateParameters();		// Called by both setBaseValue and setVibratoValue to update synth parameters
		double transeg(double outVal1, double outVal2, double concavity, double inVal, bool linear);
		
		// Variables indicating the mapping from this quality to each parameter of the PllSynth.  bool
		// variables indicate whether this quality affects a given parameter.
		
		bool useGlobalAmplitude_, globalAmplitudeLinear_, globalAmplitudeAbsolute_;
		double globalAmplitudeMin_, globalAmplitudeMax_, globalAmplitudeConcavity_;
		bool useRelativeFrequency_, relativeFrequencyLinear_, relativeFrequencyAbsolute_;
		double relativeFrequencyMin_, relativeFrequencyMax_, relativeFrequencyConcavity_;
		bool useHarmonicAmplitudes_, harmonicAmplitudesLinear_, harmonicAmplitudesAbsolute_;
		vector<double> harmonicAmplitudesMin_, harmonicAmplitudesMax_;
		double harmonicAmplitudesConcavity_;
		bool useHarmonicPhases_, harmonicPhasesLinear_, harmonicPhasesAbsolute_;
		vector<double> harmonicPhasesMin_, harmonicPhasesMax_;
		double harmonicPhasesConcavity_;
		bool useLoopGain_, loopGainLinear_, loopGainAbsolute_;
		double loopGainMin_, loopGainMax_, loopGainConcavity_;
		bool useHarmonicCentroid_, harmonicCentroidLinear_, harmonicCentroidAbsolute_, harmonicCentroidMultiply_;
		double harmonicCentroidMin_, harmonicCentroidMax_, harmonicCentroidConcavity_;
		double harmonicCentroidRoundMin_, harmonicCentroidRoundMax_;
		
		double vibratoWeight_;								// How much of an effect vibrato has on the parameter value
		bool clipVibratoLower_, clipVibratoUpper_;			// Whether to clip the effect of vibrato to a [0,1] range
		
		// Current values
		double currentBaseValue_, currentVibratoValue_;
		
		double currentGlobalAmplitude_, currentRelativeFrequency_, currentHarmonicCentroid_, currentLoopGain_;
		vector<double> currentHarmonicAmplitudes_, currentHarmonicPhases_;
	};
	
	// ******* Current state *******
	
	double centerFrequency_;
	bool useKeyDownHoldoff_, usingRawHarmonics_;
	double keyDownHoldoffTime_, keyDownHoldoffScaler_;
	int harmonicSweepRange_, harmonicSweepSpread_;
	bool usePitchBendWithHarmonics_;
	
	PllSynthQuality *intensity_;			// These update in real time according to the set methods above
	PllSynthQuality *brightness_;
	PllSynthQuality *pitch_;
	PllSynthQuality *harmonic_;
};


#endif