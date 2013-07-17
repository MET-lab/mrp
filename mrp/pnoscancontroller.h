//
//  pnoscancontroller.h
//  mrp
//
//  Created by Jeff Gregorio on 9/7/12.
//
//

#ifndef mrp_pnoscancontroller_h
#define mrp_pnoscancontroller_h

#include "midicontroller.h"
#include "note.h"
#include "realtimenote.h"

#define DEBUG_SYNTH_PARAMETERS
/*! ******************************************************************************************************************************************** //
 // ============================================================== PNOscanController ============================================================ //
 // ********************************************************************************************************************************************* //
 PNOscanController is a member of MidiController and contains all the pertinent data and methods needed to control MRP actuation using the non-
 standard MIDI polyphonic aftertouch messages sent by the QRS PNOscan.  This is accomplished using a key state machine detailed in
 PNOscanController::PAevent::updateKeyStates(), much in the same way as the PianoBarController class, but with some key differences.
 
 When the first polyphonic aftertouch message is received for a newly instantiated Note object, a nested PNOscanController::PAevent object is
 created.  Upon receipt of every subsequent polyphonic aftertouch message for this key (while the PAevent object exists), the position is stored
 in a history by PAevent::updateKeyPositionHistory(), which is immediately used by PAevent::runningMotionAnalysis()to calclate motion features
 (velocity, acceleration, avg. position, avg. velocity, avg. acceleration, peak acceleration).  Immediately following the motion analysis, the
 state machine detailed in PAevent::updateKeyStates() uses the current state and the motion features to determine if the key should change its
 state, which is handled by PNOscanController::changeKeyState().  This method then calls PNOscanController::sendKeyStateMessages() to update
 the associated RealTimeMidiNote object's synth parameters.  Upon returning to PA_kKeyStateIdle, the PAevent object is deleted. */

#define NUM_WHITE_KEYS 52
#define NUM_BLACK_KEYS 36
#define PNOSCAN_NOISE_THRESH 8

typedef long double ps_timestamp;

class MidiController;
class Note;
class RealTimeMidiNote;
class Parameter;

class PNOscanController
{
    friend class MidiController;
    class PAevent;      //! Forward declaration of PAevent class so PNOscanController can hold a map of current PAevent
    
    // ========================================================================================================================================= //
    // ------------------------------------------------------------ Enumerations --------------------------------------------------------------- //
    // ========================================================================================================================================= //
    //! Key color, used for finding the nearest white key for any given key
    enum {
        K_W = 0,
        K_B = 1
    };
    
    const short kPianoBarKeyColor[88] = {			 K_W, K_B, K_W,		// Octave 0
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 1
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 2
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 3
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 4
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 5
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 6
        K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 7
        K_W };                                                          // Octave 8
    

    //! Possible key states
    enum {
        PA_kKeyStateIdle = 0,
        PA_kKeyStatePretouch = 1,
        PA_kKeyStatePreVibrato = 2,
        PA_kKeyStateTap = 3,
        PA_kKeyStatePress = 4,
        PA_kKeyStateDown = 5,
        PA_kKeyStateAftertouch = 6,
        PA_kKeyStateAfterVibrato = 7,
        PA_kKeyStateRelease = 8,
        PA_kKeyStateDisabled = 9,
        PA_kKeyStatesLength = 10
    };
    
    /*! Key state transition thresholds are constant values for MIDI polyphonic aftertouch (assuming good calibration), rather than the thresholds
     determined during calibration used by the PianoBar */
    enum {
        PA_keyIdlePositionThreshold = 8,                //! Position below which the key is Idle used in conjunction with velocity)
        PA_keyIdlePressVelocityThreshold = 300,         /*! Average velocity above which we transition from Idle to Press, and below which we go to
                                                         Pretouch */
        PA_keyPressPositionThreshold = 45,              //! Position above which we transition from Idle/Pretouch to Press (used in conjunction with...)
        PA_keyPretouchPressVelocityThreshold = 100,     //! Velocity above which we transition from Pretouch to Press
        PA_keyVibratoCounterThreshold = 8,              /*! Number of times we need to increment the vibrato counter to trigger a PreVibrato or
                                                         AfterVibrato state */
        PA_keyAfterVibratoReleasePositionThreshold = 75,//! Position where we transition from AfterVibrato to Release
        PA_keyPreVibratoPressPositionThreshold = 32,    //! Average position above which we transition from PreVibrato to Press
        
        PA_keyDownPositionThreshold = 100,              //! Position above which a Press ends and a Down state begins
        PA_keyAftertouchPositionThreshold = 115,        //! Position above which a Down state ends and Aftertouch begins, giving us 7 aftertouch positions
        PA_keyReleasePositionThreshold = 90,            //! Position below which the key is in a Release state (should use MIDI note off message instead)
        PA_keyPressReleaseVelocityThreshold = 500,      /*! Velocity magnitude above which we go from Press to Release, and below which we go from
                                                         Press to Pretouch */
        PA_keyTapAccelerationThreshold = 500000,        /*! Peak acceleration above which the key is in a Tap state (while we're below the press
                                                         position thresh) */
        PA_keyPretouchHoldoffVelocity = 50,             //! Velocity above which we wait to generate sound
        PA_keyDownHoldoffVelocity = 10,                 //! Threshold to differentiate hammer-strike vs. silent press
    };
    
#define PA_keyIdleBounceTime 0.06                   //! Time after release when a key is bouncing
#define PA_keyVibratoTimeout 1                      //! Time before we declare a vibrato gesture done
#define PA_keyVibratoMaxFrameSpacing 1              //! Max time before which several velocity reversals will trigger a Vibrato state
#define PA_keyDownHoldoffTime 0                     //! Time after hammer strike to wait before engaging resonator
#define PA_multikeySweepMaxSpacing 0                //! How long between key events we ...???
      
    // ============================================================================================================================================= //
    // :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Public Methods ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
    // ============================================================================================================================================= //
public:
    // ========================================================================================================================================= //
    // ------------------------------------------------------- Constructor/Destructor ---------------------------------------------------------- //
    // ========================================================================================================================================= //
    //! Verbose constructor
    PNOscanController(MidiController *controller);
    
    //! Verbose destructor
    ~PNOscanController();
    
    // ========================================================================================================================================= //
    // ---------------------------------------------- Initialization and Data Member Assignment ------------------------------------------------ //
    // ========================================================================================================================================= //
    /*! Initialize the PNOscan:
     This method sends sysex messages to the PNOscan ensuring its mode, hysteresis, and trigger/release positions are set accoring to command
     line arguments, with defaults used in the absence of the command line arguments. */
    void PNOscanInitialize(RtMidiOut midiOut, int mode, int hyst, int trigger, int release);
    
    //! Set the MIDI channel the PNOscanController listens on
    void setMidiChannel(int midiChannel)    {   midiChannel_ = midiChannel; }
    
    // ============================================================================================================================================= //
    // ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Protected Methods ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
    // ============================================================================================================================================= //
protected:
    //! Timestamp is protected so it can be updated by friend class MidiController's callback
    ps_timestamp currentTimeStamp_;                                                                     //! Current timestamp from the PNOscan
    
    // ========================================================================================================================================= //
    // ---------------------------------------------------- Data Member Query Methods ---------------------------------------------------------- //
    // ========================================================================================================================================= //
    /*! Returns the current and previous key states for a given key */
    vector<int> getKeyState(unsigned int key) {  return keyStates_[key]; }
    
    /*! Returns the state machine debug mode flag */
    bool getDebugModeFlag()             {   return stateMachineDebugMode_;  }
    bool getVerboseModeFlag()           {   return PNOverbose_;             }
    int getPNOscanTriggerPosition()     {   return PNOtrigger_;             }
    
    /*! whiteKeyAbove and whiteKeyBelow return the MIDI note number of the nearest white keys to a specified MIDI note number.  These are called
     by PNOscanController::handleMultiKeyPitchBend. */
    int whiteKeyAbove(unsigned int key);
    int whiteKeyBelow(unsigned int key);
    
    //! Calls the PAnote's getNote() method for a specified key
    RealTimeMidiNote *getNoteForKey(unsigned int key);
    
    //! Initializes all previous note off times, note on times, and MIDI velocities to zero, and current and previous key states to kKeyStateIdle
    void reset(ps_timestamp cts);
    
    // ========================================================================================================================================= //
    // -------------------------------------------------------- MIDI Event Methods ------------------------------------------------------------- //
    // ========================================================================================================================================= //
    /*! Called by rtMidiCallback (in conjunction with MidiController's note on method), the midiNoteOn() and midiNoteOff() methods do not deal
     with instantiation of actual Note objects, but instead only keep track of the MIDI NOTEON and NOTEOFF messages for timing purposes
     required by the key state machine. */
	void noteOn(vector<unsigned char> *message);
	void noteOff(vector<unsigned char> *message);
    /*! Called by rtMidiCallback when PNOscan polyphonic aftertouch messages are received.  Creates a PAevent if one does not already exist, and if
     one does exist, it updates the running motion analyses and key state, and sends the key state messages to the Note object. */
    void handlePolyphonicAftertouch(vector<unsigned char> *message);
    /*! Called by PAevent::updateKeyStates (the state machine), keyStates_ */
    void changeKeyState(unsigned int key, int newState, ps_timestamp timeStamp);
    
// ============================================================================================================================================= //
// :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: Private Methods :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
// ============================================================================================================================================= //
private:
    // ========================================================================================================================================= //
    // ------------------------------------------------------------ Data Members --------------------------------------------------------------- //
    // ========================================================================================================================================= //
    MidiController *midiController_;                            //! Pointer to the MidiController used to create this PNOscanController object
    int midiChannel_;                                           //! MIDI input channel the PNOscanController listens on
    
    int PNOmode_;                                               //! PNOscan mode
    int PNOhyst_;                                               //! Hysteresis level of the PNOscan
    int PNOtrigger_;                                            //! Position at which the PNOscan sends a MIDI note on message
    int PNOrelease_;                                            //! Position at which the PNOscan sends a MIDI note off message
    
    map<unsigned int, bool> currentMidiEvents_;                 /*! Keeps track of (what should be) currently sounding notes on the main piano
                                                                 keyboard. The map's key is the actual piano key (0-87), rather than the key
                                                                 used when there are multiple MIDI channels. */
    map<unsigned int, PAevent*> currentPAevents_;               //! Holds current PAevent objects, mapped by the piano key
    map<unsigned int, vector<int> > keyStates_;                 /*! Current (keyStates_[key_][0]) and previous (keyStates_[key_][1]) states of
                                                                 all 88 keys. */
    map<unsigned int, int> lastMidiNoteOnVelocities_;           //! Holds the last MIDI velocities (from the note on message) for each key
    map<unsigned int, ps_timestamp> lastNoteOnTime_;            //! Holds the timestamp of the last NOTEON for each key
    map<unsigned int, ps_timestamp> lastNoteOffTime_;           /*! Holds the timestamp of the last NOTEOFF event for a given key, used in
                                                                 rtMidiCallback to prevent key bounce from triggering an extra note on event. */
    map<unsigned int, ps_timestamp> lastDownStateTime_;         /*! Holds the timestamp of the last time each key entered the Down state, used
                                                                 by PAevent::handleMultiKeyPitchBend()
                                                                 and PAevent::handleMultiKeyHarmonicSweep() */
    ps_timestamp lastStateUpdate_;
    
    bool displayPNOstateChanges_;
    bool PNOverbose_;                                           //! Set as a command line option.  Displays key state changes on standard output
    bool stateMachineDebugMode_;                                /*! Set as a command line option.  Prints motion analysis and state information
                                                                 each time PAevent::updateKeyStates() is called for state machine debugging.  */
    
    // ========================================================================================================================================= //
    // ----------------------------------------------------------- Sanity Checks --------------------------------------------------------------- //
    // ========================================================================================================================================= //
    //! Scans through the currentPAevents_ map and indicates for which keys PAevent objects currently exist.
    void printAllPAevents();
    //! Convert an integer key state to a string for displaying
    string kKeyStateToString(int state);
    
    
    /*! **************************************************************************************************************************************** //
     // ============================================================== PAevent ================================================================== //
     // ***************************************************************************************************************************************** //
     The QRS PNOscan sends key position as MIDI polyphonic aftertouch, which is a non-standard usage.  PNOscanController keeps a map of PAevents
     corresponding to each key on the piano keyboard.  Each key is initalized to PA_kKeyStateIdle.  When a polyphonic aftertouch message is
     received for a particular key, a PAevent object is created that keeps a history of the key's motion (including instantaneous and average
     position, velocity, and acceleration).  Each PAevent then utilizes a state machine detailed in PAevent::updateKeyStates() which uses the
     motion features to change the key state.  Upon returning to PA_kKeyStateIdle, the PAevent object is deleted until another MIDI polyphonic
     aftertouch message is received for that key.  A map of the current and previous key states for all 88 keys is held as the data member
     PNOscanController::keyStates_, and PNOscanController::sendKeyStateMessages() updates the RealTimeMidiNote object's synth parameters when
     the key state changes. */
    class PAevent
    {
        friend class Note;
        friend class RealTimeMidiNote;
        friend class Parameter;
        // ===================================================================================================================================== //
        // ::::::::::::::::::::::::::::::::::::::::::::::::::::::: Public Methods :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
        // ===================================================================================================================================== //
    public:
        // ===================================================================================================================================== //
        // --------------------------------------------------- Constructor/Destructor ---------------------------------------------------------- //
        // ===================================================================================================================================== //
        //! Verbose constructor
        PAevent(PNOscanController *PNOcontroller, RealTimeMidiNote *note, vector<unsigned char> *message, ps_timestamp currentTimestamp);
        //! Verbose destructor
        ~PAevent();
        
        // ===================================================================================================================================== //
        // ------------------------------------------------- Data Member Query Methods --------------------------------------------------------- //
        // ===================================================================================================================================== //
        Note * getNote()  {   return note_;   }
        
        // ===================================================================================================================================== //
        // ----------------------------------------- Motion Analysis and Key Gesture Recognition ----------------------------------------------- //
        // ===================================================================================================================================== //
        /*! Called by PNOscanController::rtMidiCallback() upon receipt of MIDI polyphonic aftertouch messages. Store polyphonic aftertouch data
         into a position history for a current PAevent. */
        void updateKeyPositionHistory(int position, ps_timestamp cts)  {   keyPositionHistory_[cts] = position;   }
        /*! Called by PNOscanController::handlePolyphonicAftertouch upon receipt of polyphonic aftertouch position messages, updates instantaneous
         average key position, velocity, acceleration, and peak acceleration.  If we're in state machine debug mode, we store the averages in
         maps by timestamp so we can view the entire history. */
        void runningMotionAnalysis();
        //! Called by runningMotionAnalysis.  Returns an updated average to be assigned by runningMotionAnalysis.
        double updateRunningAverage(double previousAverage, double x0, double x1, ps_timestamp t0, ps_timestamp t1);
        /*! Called by PNOscanController::handlePolyphonicAftertouch().  Updates the state associated with the PAevent object, which are held in a
         map PNOscanController::keyStates_.  This member contains the actual state machine that changes state depending on the current state and
         the motion features. */
        void updateKeyStates();
        /*! Called by sendKeyStateMessages() when the current state is PA_kKeyStatePretouch, the following two methods scan for multi-key gestures. */
        /*! Watch for the pitch bend gesture, which is triggered by a key in the Down state whose neighboring white key enters Pretouch AFTER a
         key press.  We use this gesture to bend the pitch of the "Down" note and give it a richer spectrum while disabling sound for the
         neighboring key. */
        bool handleMultiKeyPitchBend();
        /*! Watch for the harmonic sweep gesture, which is triggered by a key in the Down state and a Pretouch state one octave above the "Down"
         note.  We use this to ascend through the harmonic series of the "Down" note. */
        bool handleMultiKeyHarmonicSweep();
        
        //! Called by PNOscanController::changeKeyState().  Sends messages to the note's synths to update parameters.
        void sendKeyStateMessages();
        
        // ===================================================================================================================================== //
        // -------------------------------------------------------- Sanity Checks -------------------------------------------------------------- //
        // ===================================================================================================================================== //
        //! Print a PAevent's position history to standard output
        void printKeyPositionHistory();
        //! Print a PAevent's current position, velocity, acceleration, averages, peaks, and state to the standard output
        void printCurrentMotionFeatures();
        //! Print a PAevent's motion histories (positon, velocity, acceleration, as well as running averages) to standard output
        void printKeyMotionHistories();
        //! Write a PAevent's motion histories to a .csv file
        void writeKeyMotionHistoriesToCSV();
        
        // ===================================================================================================================================== //
        // ::::::::::::::::::::::::::::::::::::::::::::::::::::::: Private Methods ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: //
        // ===================================================================================================================================== //
    private:
        // ===================================================================================================================================== //
        // -------------------------------------------------------- Data Members --------------------------------------------------------------- //
        // ===================================================================================================================================== //
        RealTimeMidiNote *note_;                                    //! Pointer to the Note object for the PAevent's piano key
        PNOscanController *PNOcontroller_;                          //! Pointer to the PNOscanController object that created the PAevent
        MidiController *midiController_;                            //! Pointer to the main MIDI controller
        unsigned int key_;                                          //! Piano key for the PAevent
        int midiChannel_;                                           //! The associated MIDI channel
        unsigned int midiNoteNumber_;                               //! The associated MIDI note number
        float noteFreq_;                                            //! Note frequency for the MIDI note number
        int midiVelocity_;                                          //! MIDI velocity from the NOTEON message
        ps_timestamp startTime_;                                    //! Timestamp of NOTEON event
        
        map<ps_timestamp, unsigned int> keyPositionHistory_;   //! Map from ps_timestamp to instantaneous position
        map<ps_timestamp, double> keyVelocityHistory_;         //! Map from ps_timestamp to instantaneous velocity
        map<ps_timestamp, double> keyAccelerationHistory_;     //! Map from ps_timestamp to instantaneous acceleration
        
        /*! Counter used to recognize vibrato gesture.  Counter is incremented whenever the velocity exceeds a threshold and reverses direction
         within a specified time.  We transition to a vibrato state when the counter exceeds 4. */
        map<ps_timestamp, int> keyVibratoCount_;
        /*! The PreVibrato flag is set to true if we enter Pretouch from Idle, and false if we enter from a Press state.  When the flag is true, we
         look for a negative velocity to increment the vibrato counter.  When it is false, we look for a positive velocity. */
        bool keyPreVibratoFlag_;
        /*! The AfterVibrato flag is set as true when we enter the Down state from a Press.  When the flag is true, we look for a negative velocity
         to increment the vibrato counter.  When it is false, we look for a positive velocity. */
        bool keyAfterVibratoFlag_;
        
        /** The following physical parameters are calculated from the entire length of PAeventPositionHistory_ and updated after each new polyphonic
         aftertouch message. */
        ps_timestamp PAdeltaTime_;                  //! Time since last polyphonic aftertouch message
        double avgKeyPosition_;                     //! Average key position
        double avgKeyVelocity_;                     //! Average key velocity
        double avgKeyAcceleration_;                 //! Average key acceleration
        double peakKeyAcceleration_;                //! Highest acceleration in position history
        
        /*! The following maps are not necessary for the operation of the state machine, which operates on the above instantaneous values, but are
         assigned if the PNOscanController is in its state machine debug mode set by PNOscanController::stateMachineDebugMode_.  They record the
         entire motion feature and state change histories of a PAevent for printing to the terminal or a .csv file. */
        map<ps_timestamp, double> avgKeyPositionHistory_;
        map<ps_timestamp, double> avgKeyVelocityHistory_;
        map<ps_timestamp, double> avgKeyAccelerationHistory_;
        map<ps_timestamp, double> peakKeyAccelerationHistory_;
        map<ps_timestamp, string> keyStateChangeHistory_;
        // ************************************************************************************************************************************* //
    };
};


#endif
