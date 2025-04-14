/***
   MIDI FM synthesiser
   VL, 2025.
***********************/
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <portmidi.h>
#include <porttime.h>
#include <portaudio.h>

#define TYPEMASK 0xF0   // MIDI MSG TYPE MASK
#define MD_NOTEON 0x90  // MIDI NOTEON MSG
#define MD_NOTEOFF 0x80 // MIDI NOTEOFF MSG
#define MD_CC 0xB0      // MIDI CC MSG
#define SR 44100        // SAMPLING RATE
#define BUFSIZE 128     // BUFFER SIZE
#define MAX_INDX 5      // max index of mod
#define TABSIZE 10000   // table size
static int run_flag = 0; // run flag

// Oscillator 
struct Osc {
  
  double fr;
  double amp;
  const double *table;
  unsigned int size;
  double ph;
  double *s;
  unsigned int vsize;
  double sr;
  
  Osc(double a, double f, const double *t, unsigned int sz, 
      double phs = 0., unsigned int vsz = BUFSIZE, 
      double esr = SR) : 
    amp(a), fr(f), table(t), size(sz), ph(phs),
    s(new double[vsz]), vsize(vsz), sr(esr) { };  
           
  ~Osc() { delete[] s; }

  // processing core
  const double *process(double a, double f, const double *fm = nullptr){
    for(int i = 0; i < vsize; i++){
      s[i] = amp * table[(int)ph];
      ph += size * (fr + (fm ? fm[i] : 0.)) / sr;
      while(ph >= size) ph  -= size;
      while(ph < 0) ph += size;
    }
    return s; 
  }

  // interface for FM input
  const double *process(const double* fm) {
    return process(amp, fr, fm);
  }  

  // interface for fixed amp, freq
  const double *process(){
    return process(amp, fr);
  }
};


// envelope
struct Envel {
  double incr;
  double decr;
  double sus;
  double relfac;
  double *s;
  unsigned int vsize;
  double sr;
  double amp;
  bool rel;
  bool att;

  Envel(double atts, double decs, double susl, double rels,
         unsigned int vsz = BUFSIZE, double esr = SR) :
                           incr(1./(atts*esr)),
                           decr((susl - 1.)/(decs*esr)),
                           sus(susl), relfac(pow(0.001, 1./(rels*esr))),
                           s(new double[vsz]),
                           vsize(vsz), sr(esr), amp(0.),
                           rel(false), att(true)
                           { };

  ~Envel() { delete[] s; }
  
  const double *process(const double *sig) {
    double a = amp;
    for(int i = 0; i < vsize; i++) {
      if(!rel) {
       if(a <= 1. && att) {
         a += incr; 
       }
       else if(a > sus) {
         a += decr;
         att = false;
       }
       else a = sus;
      } else {
        a *= relfac;
      }
      s[i] = a*sig[i]; 
    }
    amp = a;
    return s;  
  }

  void onset() { att = true; rel = false; };
  void release() { rel = true; };
  
};


// FM Synth
struct Synth {
  int dev;
  Osc car, mod;
  Envel adsr;
  double tab[TABSIZE];
  Synth() : dev(0), car(0.5, 0., tab, TABSIZE),
            mod(0., 0., tab, TABSIZE),
            adsr(0.002,0.05,0.7,1) {
    for(int i =0 ; i < TABSIZE; i++)
      tab[i] = sin(i*2*M_PI/TABSIZE);

  }
  
  void SetFreq(double f) { mod.fr = car.fr =  f; }
  void SetAmp(double a) { car.amp =  a; }
  void SetIndx(double z) { mod.amp = z*mod.fr; }
  
  const double *process() {
    return adsr.process(car.process(mod.process()));
  }
};

// Audio output
struct AudioOut {
  
  PaStream *stream;
  
  AudioOut(Synth *synth) {
    PaError err;
    PaStreamParameters param;
    int dev;
  
    Pa_Initialize(); 
    dev = Pa_GetDefaultOutputDevice();
    param.device = (PaDeviceIndex) dev;
    param.channelCount = 1;
    param.sampleFormat = paFloat32;
    param.suggestedLatency = (PaTime)
      (BUFSIZE/(double)SR);
    param.hostApiSpecificStreamInfo = NULL;
    err = Pa_OpenStream(&stream,NULL,&param, SR, BUFSIZE, paNoFlag, callback,
                        synth);
    if(err != paNoError) {
      printf("Error opening audio output\n");
      stream = nullptr;
    } 
  }

  ~AudioOut() {
    if(stream) {
      Pa_StopStream(stream);
      Pa_CloseStream(stream);
    }
    Pa_Terminate();
  }

  int start() {
    if(stream) {
      return Pa_StartStream(stream);
    }
    else return 1;
  }

  // audio synthesis callback (static member)
  static int callback(const void *input, void *output,
              unsigned long frameCount,
              const PaStreamCallbackTimeInfo *timeInfo,
              PaStreamCallbackFlags statusFlags, void *userData){
    int i;
    Synth *synth = (Synth *) userData;
    float *outp = (float *) output;
    const double *s = synth->process();
  
    // copy data to output
    for(i=0; i < frameCount; i++) {
      outp[i] = (float) s[i];
    }
    return paContinue;
  }   
};

// MIDI input
struct MidiIn {
  
  PortMidiStream *mstream;
  Synth *synth;
  
  MidiIn(Synth *p) : synth(p) {
    int cnt;
    const PmDeviceInfo *info;
    Pm_Initialize();
    // get input MIDI device list
    cnt = Pm_CountDevices();
    if(cnt == 0) {
      printf("No available MIDI devices\n");
      return;
    }
    // select MIDI input device
    for(int i=0; i < cnt; i++){
      info = Pm_GetDeviceInfo(i);
      if(info->input)
        printf("%d: %s \n", i, info->name);
    }
    printf("choose device: ");
    scanf("%d", &synth->dev);
  }

  ~MidiIn() {
    if(mstream) Pm_Close(mstream);
    Pm_Terminate();
  }

  void listen() {
    PmError retval;  
    PmEvent msg[32];
    unsigned char note;
    int cnt;
    
    // start PortTime
    Pt_Start(1, NULL, NULL);
    // open MIDI input stream
    retval = Pm_OpenInput(&mstream, synth->dev, NULL, 512L, NULL,NULL);
    if(retval != pmNoError) {   
      printf("error: %s \n", Pm_GetErrorText(retval));
      mstream = nullptr;
      return;
    }
    run_flag = 1;
    while(run_flag){
      // poll MIDI data stream for messages
      if(Pm_Poll(mstream)) {
        unsigned char data1, data2, status;
        cnt = Pm_Read(mstream, msg, 32);
        for(int i=0; i<cnt; i++) {
          // get MIDI bytes
          status = Pm_MessageStatus(msg[i].message);
          data1 = Pm_MessageData1(msg[i].message);
          data2 = Pm_MessageData2(msg[i].message);
          // NOTEON
          if((status & TYPEMASK) == MD_NOTEON &&
             data2 != 0) {
            // MIDI NN to Hz
            synth->SetFreq(440.*pow(2., (data1 - 69.)/12));
            synth->adsr.onset();
            note = data1;
          }
          // NOTEOFF
          else if(((status & TYPEMASK) == MD_NOTEOFF
                   || ((status & TYPEMASK) == MD_NOTEON &&
                       data2 == 0))) {
            if(note == data1) {
              // update current note
              note = data1;
              synth->adsr.release();
            }  
          }
          else if((status & TYPEMASK) == MD_CC) {
            // continuous control
            // exit on CC 50
            if(data1 == 50 && data2 == 1) break;
            // index from CC01, mod wheel 
            if(data1 == 1) {
              synth->SetIndx(MAX_INDX*data2/128.);
            }
          }
        }
      }
    }
  }
};

// signal handler
static void interrupt_handler(int n) {
  run_flag = 0;
}

// main program
int main(int argc, const char *argv[]) {
  Synth synth;
  MidiIn midiIn(&synth);
  AudioOut audioOut(&synth);
  signal(SIGINT, interrupt_handler);
  
  if(audioOut.start() == 0) {
    printf("running...\n");
    midiIn.listen();
  }
  
  printf("\n...exiting.\n");
  return 0;
}



