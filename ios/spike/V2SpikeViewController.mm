#import "V2Spike.h"
#import "V2SpikeEngine.h"

#import <AVFoundation/AVFoundation.h>

@interface V2SpikeDisplayView : UIView
@property (nonatomic, readonly) AVSampleBufferDisplayLayer* displayLayer;
@end

@implementation V2SpikeDisplayView
+ (Class)layerClass { return [AVSampleBufferDisplayLayer class]; }
- (AVSampleBufferDisplayLayer*)displayLayer {
    return (AVSampleBufferDisplayLayer*)self.layer;
}
@end

@interface V2SpikeViewController ()
@property (nonatomic, strong) V2SpikeDisplayView* displayView;
@property (nonatomic, strong) UILabel* statusLabel;
@property (nonatomic, strong) UIButton* doneButton;
@property (nonatomic, strong) AVSampleBufferAudioRenderer* audioRenderer;
@property (nonatomic, strong) AVSampleBufferRenderSynchronizer* synchronizer;
@property (nonatomic, strong) V2SpikeEngine* engine;
@property (nonatomic, assign) BOOL didReportCompletion;
@end

@implementation V2SpikeViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    self.displayView = [[V2SpikeDisplayView alloc] init];
    self.displayView.translatesAutoresizingMaskIntoConstraints = NO;
    self.displayView.displayLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    [self.view addSubview:self.displayView];

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.textColor = [UIColor whiteColor];
    self.statusLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.statusLabel.numberOfLines = 0;
    self.statusLabel.text = @"Loading…";
    [self.view addSubview:self.statusLabel];

    self.doneButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.doneButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.doneButton setTitle:@"Done" forState:UIControlStateNormal];
    [self.doneButton setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    self.doneButton.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    [self.doneButton addTarget:self action:@selector(doneTapped) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.doneButton];

    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.displayView.topAnchor constraintEqualToAnchor:safe.topAnchor constant:50],
        [self.displayView.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [self.displayView.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [self.displayView.heightAnchor constraintEqualToAnchor:self.displayView.widthAnchor multiplier:9.0/16.0],

        [self.statusLabel.topAnchor constraintEqualToAnchor:self.displayView.bottomAnchor constant:16],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [self.statusLabel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],

        [self.doneButton.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [self.doneButton.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16]
    ]];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (self.engine) return;
    [self setupAudioSession];
    [self startEngine];
}

- (void)setupAudioSession {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* err = nil;
    [session setCategory:AVAudioSessionCategoryPlayback
                    mode:AVAudioSessionModeDefault
                 options:0
                   error:&err];
    if (err) NSLog(@"[V2Spike] AVAudioSession setCategory error: %@", err);
    [session setActive:YES error:&err];
    if (err) NSLog(@"[V2Spike] AVAudioSession setActive error: %@", err);
}

- (void)startEngine {
    self.audioRenderer = [[AVSampleBufferAudioRenderer alloc] init];
    self.synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
    [self.synchronizer addRenderer:self.audioRenderer];
    [self.synchronizer addRenderer:self.displayView.displayLayer];

    self.engine = [[V2SpikeEngine alloc] initWithDisplayLayer:self.displayView.displayLayer
                                                 audioRenderer:self.audioRenderer
                                                  synchronizer:self.synchronizer];
    self.statusLabel.text = @"Demuxing fixture…";
    __weak typeof(self) weakSelf = self;
    [self.engine playFixtureWithCompletion:^(NSError* error) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        if (error) {
            strongSelf.statusLabel.text = [NSString stringWithFormat:@"Failed: %@", error.localizedDescription];
            [strongSelf reportCompletion:V2SpikeResultFailed error:error];
        } else {
            strongSelf.statusLabel.text = [NSString stringWithFormat:
                @"Played: %lu audio / %lu video / %.2fs",
                (unsigned long)strongSelf.engine.audioPacketsEnqueued,
                (unsigned long)strongSelf.engine.videoPacketsEnqueued,
                strongSelf.engine.fixtureDurationSeconds];
            [strongSelf reportCompletion:V2SpikeResultPlayed error:nil];
        }
    }];
    self.statusLabel.text = @"Decoding + enqueuing…";
}

- (void)doneTapped {
    [self.engine stop];
    [self reportCompletion:V2SpikeResultDismissed error:nil];
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)reportCompletion:(V2SpikeResult)result error:(NSError*)error {
    if (self.didReportCompletion) return;
    self.didReportCompletion = YES;
    if (self.completionHandler) self.completionHandler(result, error);
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [self.engine stop];
}

@end
