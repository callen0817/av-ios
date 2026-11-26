#pragma once

#import <Foundation/Foundation.h>
#import <MultipeerConnectivity/MultipeerConnectivity.h>
#include <functional>
#include <string>
#include <vector>

// Apple MultipeerConnectivity transport for zero-router iOS/iPadOS <-> iOS/iPadOS C-SLAM
// Uses Bluetooth + Bonjour for discovery and peer-to-peer Wi-Fi (AWDL) for high-speed keyframe sync.
@interface MultipeerCollabTransport : NSObject <MCSessionDelegate, MCNearbyServiceAdvertiserDelegate, MCNearbyServiceBrowserDelegate>

@property (nonatomic, assign, readonly) BOOL isRunning;
@property (nonatomic, copy, readonly) NSString *localDisplayName;

- (instancetype)initWithDisplayName:(NSString *)displayName;

- (void)setPacketCallback:(std::function<void(const uint8_t *, size_t, const std::string &)>)packetCb
             peerCallback:(std::function<void(const std::string &, const std::string &)>)peerCb;

- (BOOL)start;
- (void)stop;

- (void)broadcastData:(const std::vector<uint8_t> &)payload reliable:(BOOL)reliable;
- (NSArray<NSString *> *)connectedPeerNames;

@end
