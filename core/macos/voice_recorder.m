// Basecamp Voice Recorder (macOS): records the microphone to a 16 kHz mono
// 16-bit WAV until a stop file appears (or 120 s pass).
//
// It is its own little app (docs/macos.md): Basecamp's macOS bundle declares
// no microphone use, and macOS kills any process of Basecamp's that opens the
// microphone. Launched with `open`, this app is responsible for itself and
// asks for permission under its own name.
//
//   BasecampVoiceRecorder <out.wav> <stop-file>
// Writes <out.wav>.started when recording, <out.wav>.done when finished, or
// <out.wav>.err with a sentence for the user.
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

static void note(NSString* path, NSString* text) {
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc < 3) return 2;
        NSString* out = [NSString stringWithUTF8String:argv[1]];
        NSString* stop = [NSString stringWithUTF8String:argv[2]];
        NSString* err = [out stringByAppendingString:@".err"];
        NSFileManager* fm = [NSFileManager defaultManager];

        // Ask once; macOS shows "Basecamp Voice Recorder would like to access the microphone".
        __block BOOL granted = NO;
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL ok) {
            granted = ok;
            dispatch_semaphore_signal(answered);
        }];
        dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC));
        if (!granted) {
            note(err, @"Microphone access is not allowed. Allow \"Basecamp Voice Recorder\" in System Settings > Privacy & Security > Microphone, then try again.");
            return 1;
        }

        NSDictionary* settings = @{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVSampleRateKey: @16000.0,
            AVNumberOfChannelsKey: @1,
            AVLinearPCMBitDepthKey: @16,
            AVLinearPCMIsFloatKey: @NO,
            AVLinearPCMIsBigEndianKey: @NO,
        };
        NSError* e = nil;
        AVAudioRecorder* rec = [[AVAudioRecorder alloc] initWithURL:[NSURL fileURLWithPath:out] settings:settings error:&e];
        if (!rec || ![rec record]) {
            note(err, [NSString stringWithFormat:@"Could not start recording: %@", e ? e.localizedDescription : @"no microphone?"]);
            return 1;
        }
        note([out stringByAppendingString:@".started"], @"");

        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:120];
        while (![fm fileExistsAtPath:stop] && [deadline timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        [rec stop];
        note([out stringByAppendingString:@".done"], @"");
        return 0;
    }
}
