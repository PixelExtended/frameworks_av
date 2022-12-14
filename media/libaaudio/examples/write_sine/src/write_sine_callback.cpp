/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Play sine waves using an AAudio callback.
// If a disconnection occurs then reopen the stream on the new device.

#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <aaudio/AAudio.h>

#include "AAudioExampleUtils.h"
#include "AAudioSimplePlayer.h"
#include "AAudioArgsParser.h"

#define APP_VERSION  "0.2.1"

static constexpr int32_t kDefaultHangTimeMSec = 10;
static constexpr int32_t kWorkPeriodSeconds = 6;
/**
 * Open stream, play some sine waves, then close the stream.
 *
 * @param argParser
 * @return AAUDIO_OK or negative error code
 */
static aaudio_result_t testOpenPlayClose(AAudioArgsParser &argParser,
                                         int32_t loopCount,
                                         int32_t prefixToneMsec,
                                         int32_t hangTimeMSec,
                                         int     cpuAffinity,
                                         double  lowWorkLoad,
                                         double  highWorkLoad,
                                         int32_t workPeriodSeconds)
{
    SineThreadedData_t myData;
    AAudioSimplePlayer &player = myData.simplePlayer;
    aaudio_result_t    result = AAUDIO_OK;
    bool               disconnected = false;
    bool               bailOut = false;
    int64_t            startedAtNanos;

    printf("----------------------- run complete test --------------------------\n");
    myData.schedulerChecked = false;
    myData.callbackCount = 0;
    myData.hangTimeMSec = hangTimeMSec; // test AAudioStream_getXRunCount()
    myData.cpuAffinity = cpuAffinity;

    result = player.open(argParser,
                         SimplePlayerDataCallbackProc,
                         SimplePlayerErrorCallbackProc,
                         &myData);
    if (result != AAUDIO_OK) {
        fprintf(stderr, "ERROR -  player.open() returned %s\n",
                AAudio_convertResultToText(result));
        goto error;
    }

    argParser.compareWithStream(player.getStream());

    myData.sampleRate = player.getSampleRate();
    myData.prefixToneFrames = prefixToneMsec * myData.sampleRate / 1000;
    if (myData.prefixToneFrames > 0) {
        myData.setupSineBlip();
    } else {
        myData.setupSineSweeps();
    }

#if 0
    //  writes not allowed for callback streams
    result = player.prime(); // FIXME crashes AudioTrack.cpp
    if (result != AAUDIO_OK) {
        fprintf(stderr, "ERROR - player.prime() returned %d\n", result);
        goto error;
    }
#endif

    for (int loopIndex = 0; loopIndex < loopCount; loopIndex++) {
        // Only play data on every other loop so we can hear if there is stale data.
        double amplitude;
        int32_t durationSeconds;
        if ((loopIndex & 1) == 0) {
            printf("--------------- SINE ------\n");
            amplitude = 0.2;
            durationSeconds = argParser.getDurationSeconds();
        } else {
            printf("--------------- QUIET -----\n");
            amplitude = 0.0;
            durationSeconds = 2; // just wait briefly when quiet
        }
        for (int i = 0; i < MAX_CHANNELS; ++i) {
            myData.sineOscillators[i].setAmplitude(amplitude);
        }

        result = player.start();
        if (result != AAUDIO_OK) {
            fprintf(stderr, "ERROR - player.start() returned %d\n", result);
            goto error;
        }

        // Play a sine wave in the background.
        printf("Monitor for %d seconds while audio plays in a callback thread. %d of %d, %d\n",
               argParser.getDurationSeconds(), (loopIndex + 1), loopCount, workPeriodSeconds);
        startedAtNanos = getNanoseconds(CLOCK_MONOTONIC);
        for (int second = 0; second < durationSeconds; second++) {
            // Sleep a while. Wake up early if there is an error, for example a DISCONNECT.
            myData.waker.wait(AAUDIO_OK, NANOS_PER_SECOND);
            int64_t millis =
                    (getNanoseconds(CLOCK_MONOTONIC) - startedAtNanos) / NANOS_PER_MILLISECOND;
            result = myData.waker.get();
            const int32_t framesWritten = (int32_t) AAudioStream_getFramesWritten(player.getStream());
            const int32_t framesRead = (int32_t) AAudioStream_getFramesRead(player.getStream());
            const int32_t xruns = AAudioStream_getXRunCount(player.getStream());
            myData.workload = ((second % (2 * workPeriodSeconds)) < workPeriodSeconds)
                    ? lowWorkLoad : highWorkLoad;
            printf(" waker result = %d, at %6d millis"
                   ", second = %3d, frames written %8d - read %8d = %8d"
                   ", work = %5.1f, underruns = %d\n",
                   result, (int) millis,
                   second,
                   framesWritten,
                   framesRead,
                   framesWritten - framesRead,
                   myData.workload,
                   xruns);
            if (result != AAUDIO_OK) {
                disconnected = (result == AAUDIO_ERROR_DISCONNECTED);
                bailOut = true;
                break;
            }
        }
        printf("AAudio result = %d = %s\n", result, AAudio_convertResultToText(result));

        // Alternate between using stop or pause for each sine/quiet pair.
        // Repeat this pattern: {sine-stop-quiet-stop-sine-pause-quiet-pause}
        if ((loopIndex & 2) == 0) {
            printf("STOP, callback # = %d\n", myData.callbackCount);
            result = player.stop();
        } else {
            printf("PAUSE/FLUSH, callback # = %d\n", myData.callbackCount);
            result = player.pause();
            if (result != AAUDIO_OK) {
                goto error;
            }
            result = player.waitUntilPaused();
            if (result != AAUDIO_OK) {
                goto error;
            }
            result = player.flush();
        }
        if (result != AAUDIO_OK) {
            goto error;
        }

        if (bailOut) {
            break;
        }

        {
            aaudio_stream_state_t state = AAudioStream_getState(player.getStream());
            aaudio_stream_state_t finalState = AAUDIO_STREAM_STATE_UNINITIALIZED;
            int64_t timeoutNanos = 2000 * NANOS_PER_MILLISECOND;
            result = AAudioStream_waitForStateChange(player.getStream(), state,
                                                     &finalState, timeoutNanos);
            printf("waitForStateChange returns %s, state = %s\n",
                   AAudio_convertResultToText(result),
                   AAudio_convertStreamStateToText(finalState));
            int64_t written = AAudioStream_getFramesWritten(player.getStream());
            int64_t read = AAudioStream_getFramesRead(player.getStream());
            printf("   framesWritten = %lld, framesRead = %lld, diff = %d\n",
                   (long long) written,
                   (long long) read,
                   (int) (written - read));
        }

    }

    printf("call close()\n");
    result = player.close();
    if (result != AAUDIO_OK) {
        goto error;
    }

    for (int i = 0; i < myData.timestampCount; i++) {
        Timestamp *timestamp = &myData.timestamps[i];
        bool retro = (i > 0 &&
                      ((timestamp->position < (timestamp - 1)->position)
                       || ((timestamp->nanoseconds < (timestamp - 1)->nanoseconds))));
        const char *message = retro ? "  <= RETROGRADE!" : "";
        printf("Timestamp %3d : %8lld, %8lld %s\n", i,
               (long long) timestamp->position,
               (long long) timestamp->nanoseconds,
               message);
    }

    if (myData.schedulerChecked) {
        printf("scheduler = 0x%08x, SCHED_FIFO = 0x%08X\n",
               myData.scheduler,
               SCHED_FIFO);
    }

    printf("min numFrames = %8d\n", (int) myData.minNumFrames);
    printf("max numFrames = %8d\n", (int) myData.maxNumFrames);

    printf("SUCCESS\n");
error:
    player.close();
    return disconnected ? AAUDIO_ERROR_DISCONNECTED : result;
}

static void usage() {
    AAudioArgsParser::usage();
    printf("      -l{count} loopCount start/stop, every other one is silent\n");
    printf("      -t{msec}  play a high pitched tone at the beginning\n");
    printf("      -w{workload}  set base workload, default 0.0\n");
    printf("      -W{workload}  alternate between this higher workload and base workload\n");
    printf("      -Z{duration}  number of seconds to spend at each workload, default = %d\n",
           kWorkPeriodSeconds);
    printf("      -a{cpu}   set CPU affinity, default none\n");
    printf("      -h{msec}  force periodic underruns by hanging in callback\n");
    printf("                If no value specified then %d used.\n",
            kDefaultHangTimeMSec);
}

int main(int argc, const char **argv)
{
    AAudioArgsParser   argParser;
    aaudio_result_t    result;
    int32_t            loopCount = 1;
    int32_t            prefixToneMsec = 0;
    int32_t            hangTimeMSec = 0;
    int                cpuAffinity = -1;
    double             lowWorkLoad = 0.0;
    double             highWorkLoad = -1.0;
    int32_t            workPeriodSeconds = kWorkPeriodSeconds;

    // Make printf print immediately so that debug info is not stuck
    // in a buffer if we hang or crash.
    setvbuf(stdout, nullptr, _IONBF, (size_t) 0);

    printf("%s - Play a sine sweep using an AAudio callback V%s\n",
        argv[0], APP_VERSION);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (argParser.parseArg(arg)) {
            // Handle options that are not handled by the ArgParser
            if (arg[0] == '-') {
                char option = arg[1];
                switch (option) {
                    case 'a':
                        cpuAffinity = atoi(&arg[2]);
                        break;
                    case 'l':
                        loopCount = atoi(&arg[2]);
                        break;
                    case 't':
                        prefixToneMsec = atoi(&arg[2]);
                        break;
                    case 'h':
                        hangTimeMSec = (arg[2]) // value specified?
                                ? atoi(&arg[2])
                                : kDefaultHangTimeMSec;
                        break;
                    case 'w':
                        lowWorkLoad = atof(&arg[2]);
                        break;
                    case 'W':
                        highWorkLoad = atof(&arg[2]);
                        break;
                    case 'Z':
                        workPeriodSeconds = atoi(&arg[2]);
                        break;
                    default:
                        usage();
                        exit(EXIT_FAILURE);
                        break;
                }
            } else {
                usage();
                exit(EXIT_FAILURE);
                break;
            }
        }
    }

    if (highWorkLoad > 0) {
        if (highWorkLoad < lowWorkLoad) {
            printf("ERROR - -W%f workload lower than -w%f workload", highWorkLoad, lowWorkLoad);
            return EXIT_FAILURE;
        }
    } else {
        highWorkLoad = lowWorkLoad; // high not specified so use low
    }

    // Keep looping until we can complete the test without disconnecting.
    while((result = testOpenPlayClose(argParser, loopCount,
            prefixToneMsec, hangTimeMSec,
            cpuAffinity,
            lowWorkLoad, highWorkLoad,
            workPeriodSeconds))
            == AAUDIO_ERROR_DISCONNECTED);

    return (result) ? EXIT_FAILURE : EXIT_SUCCESS;
}
