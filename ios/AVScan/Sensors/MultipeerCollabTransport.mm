#import "MultipeerCollabTransport.h"
#include <mutex>

static NSString *const kAvCslamServiceType = @"avcslam";

@implementation MultipeerCollabTransport {
    MCPeerID *_localPeerId;
    MCSession *_session;
    MCNearbyServiceAdvertiser *_advertiser;
    MCNearbyServiceBrowser *_browser;

    std::mutex _cbMutex;
    std::function<void(const uint8_t *, size_t, const std::string &)> _onPacket;
    std::function<void(const std::string &, const std::string &)> _onPeer;
}

- (instancetype)initWithDisplayName:(NSString *)displayName {
    self = [super init];
    if (self) {
        NSString *safeName = (displayName.length > 0) ? displayName : @"AV-iPad";
        if (safeName.length > 32) {
            safeName = [safeName substringToIndex:32];
        }
        _localDisplayName = [safeName copy];
        _isRunning = NO;
    }
    return self;
}

- (void)setPacketCallback:(std::function<void(const uint8_t *, size_t, const std::string &)>)packetCb
             peerCallback:(std::function<void(const std::string &, const std::string &)>)peerCb {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _onPacket = std::move(packetCb);
    _onPeer = std::move(peerCb);
}

- (BOOL)start {
    if (_isRunning) return YES;

    _localPeerId = [[MCPeerID alloc] initWithDisplayName:_localDisplayName];
    _session = [[MCSession alloc] initWithPeer:_localPeerId
                              securityIdentity:nil
                          encryptionPreference:MCEncryptionOptional];
    _session.delegate = self;

    _advertiser = [[MCNearbyServiceAdvertiser alloc] initWithPeer:_localPeerId
                                                    discoveryInfo:@{@"app": @"AV_IOS", @"proto": @"1"}
                                                      serviceType:kAvCslamServiceType];
    _advertiser.delegate = self;

    _browser = [[MCNearbyServiceBrowser alloc] initWithPeer:_localPeerId serviceType:kAvCslamServiceType];
    _browser.delegate = self;

    [_advertiser startAdvertisingPeer];
    [_browser startBrowsingForPeers];
    _isRunning = YES;
    return YES;
}

- (void)stop {
    if (!_isRunning) return;
    [_advertiser stopAdvertisingPeer];
    [_browser stopBrowsingForPeers];
    [_session disconnect];
    _advertiser = nil;
    _browser = nil;
    _session = nil;
    _localPeerId = nil;
    _isRunning = NO;
}

- (void)broadcastData:(const std::vector<uint8_t> &)payload reliable:(BOOL)reliable {
    if (!_isRunning || !_session || payload.empty()) return;
    NSArray<MCPeerID *> *peers = _session.connectedPeers;
    if (peers.count == 0) return;

    NSData *data = [NSData dataWithBytes:payload.data() length:payload.size()];
    NSError *err = nil;
    [_session sendData:data
               toPeers:peers
              withMode:(reliable ? MCSessionSendDataReliable : MCSessionSendDataUnreliable)
                 error:&err];
}

- (NSArray<NSString *> *)connectedPeerNames {
    if (!_session) return @[];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (MCPeerID *p in _session.connectedPeers) {
        [names addObject:p.displayName];
    }
    return names;
}

#pragma mark - MCNearbyServiceBrowserDelegate

- (void)browser:(MCNearbyServiceBrowser *)browser
      foundPeer:(MCPeerID *)peerID
withDiscoveryInfo:(NSDictionary<NSString *, NSString *> *)info {
    (void)info;
    if (!_session || [peerID.displayName isEqualToString:_localPeerId.displayName]) return;
    // Automatically invite discovered AV C-SLAM peers (Bluetooth + AWDL P2P Wi-Fi)
    [browser invitePeer:peerID toSession:_session withContext:nil timeout:15.0];
}

- (void)browser:(MCNearbyServiceBrowser *)browser lostPeer:(MCPeerID *)peerID {
    (void)browser;
    (void)peerID;
}

#pragma mark - MCNearbyServiceAdvertiserDelegate

- (void)advertiser:(MCNearbyServiceAdvertiser *)advertiser
didReceiveInvitationFromPeer:(MCPeerID *)peerID
       withContext:(NSData *)context
 invitationHandler:(void (^)(BOOL, MCSession *))invitationHandler {
    (void)advertiser;
    (void)peerID;
    (void)context;
    if (invitationHandler && _session) {
        invitationHandler(YES, _session);
    }
}

#pragma mark - MCSessionDelegate

- (void)session:(MCSession *)session peer:(MCPeerID *)peerID didChangeState:(MCSessionState)state {
    (void)session;
    if (state == MCSessionStateConnected) {
        std::function<void(const std::string &, const std::string &)> cb;
        {
            std::lock_guard<std::mutex> lock(_cbMutex);
            cb = _onPeer;
        }
        if (cb) {
            std::string name = peerID.displayName.UTF8String ?: "iOS Peer";
            cb("mc_" + name, name);
        }
    }
}

- (void)session:(MCSession *)session didReceiveData:(NSData *)data fromPeer:(MCPeerID *)peerID {
    (void)session;
    if (data.length == 0) return;
    std::function<void(const uint8_t *, size_t, const std::string &)> cb;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        cb = _onPacket;
    }
    if (cb) {
        std::string label = std::string("P2P:") + (peerID.displayName.UTF8String ?: "iOS");
        cb(reinterpret_cast<const uint8_t *>(data.bytes), static_cast<size_t>(data.length), label);
    }
}

- (void)session:(MCSession *)session didReceiveStream:(NSInputStream *)stream withName:(NSString *)streamName fromPeer:(MCPeerID *)peerID {
    (void)session; (void)stream; (void)streamName; (void)peerID;
}

- (void)session:(MCSession *)session didStartReceivingResourceWithName:(NSString *)resourceName fromPeer:(MCPeerID *)peerID withProgress:(NSProgress *)progress {
    (void)session; (void)resourceName; (void)peerID; (void)progress;
}

- (void)session:(MCSession *)session didFinishReceivingResourceWithName:(NSString *)resourceName fromPeer:(MCPeerID *)peerID atURL:(NSURL *)localURL withError:(NSError *)error {
    (void)session; (void)resourceName; (void)peerID; (void)localURL; (void)error;
}

@end
