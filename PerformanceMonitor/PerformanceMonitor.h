//
//  PerformanceMonitor.h
//  SuperApp
//

#import <Foundation/Foundation.h>

@interface PerformanceMonitor : NSObject

+ (instancetype)sharedInstance;

- (void)start;
- (void)stop;

@end
