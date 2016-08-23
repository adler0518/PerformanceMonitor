# PerformanceMonitor
iOS实时性能监控

在移动设备上开发软件,性能一直是我们最为关心的话题之一,我们作为程序员除了需要努力提高代码质量之外,及时发现和监控软件中那些造成性能低下的”罪魁祸首”也是我们神圣的职责.

众所周知,iOS平台因为UIKit本身的特性,需要将所有的UI操作都放在主线程执行,所以也造成不少程序员都习惯将一些线程安全性不确定的逻辑,以及其它线程结束后的汇总工作等等放到了主线,所以主线程中包含的这些大量计算、IO、绘制都有可能造成卡顿.

在Xcode中已经集成了非常方便的调试工具Instruments,它可以帮助我们在开发测试阶段分析软件运行的性能消耗,但一款软件经过测试流程和实验室分析肯定是不够的,在正式环境中由大量用户在使用过程中监控、分析到的数据更能解决一些隐藏的问题.

寻找卡顿的切入点

监控卡顿,最直接就是找到主线程都在干些啥玩意儿.我们知道一个线程的消息事件处理都是依赖于NSRunLoop来驱动,所以要知道线程正在调用什么方法,就需要从NSRunLoop来入手.CFRunLoop的代码是开源,可以在此处查阅到源代码http://opensource.apple.com/source/CF/CF-1151.16/CFRunLoop.c,其中核心方法CFRunLoopRun简化后的主要逻辑大概是这样的:


int32_t __CFRunLoopRun()
{
    //通知即将进入runloop
    __CFRunLoopDoObservers(KCFRunLoopEntry);
    
    do
    {
        // 通知将要处理timer和source
        __CFRunLoopDoObservers(kCFRunLoopBeforeTimers);
        __CFRunLoopDoObservers(kCFRunLoopBeforeSources);
        
        __CFRunLoopDoBlocks();  //处理非延迟的主线程调用
        __CFRunLoopDoSource0(); //处理UIEvent事件
        
        //GCD dispatch main queue
        CheckIfExistMessagesInMainDispatchQueue();
        
        // 即将进入休眠
        __CFRunLoopDoObservers(kCFRunLoopBeforeWaiting);
        
        // 等待内核mach_msg事件
        mach_port_t wakeUpPort = SleepAndWaitForWakingUpPorts();
        
        // Zzz...
        
        // 从等待中醒来
        __CFRunLoopDoObservers(kCFRunLoopAfterWaiting);
        
        // 处理因timer的唤醒
        if (wakeUpPort == timerPort)
            __CFRunLoopDoTimers();
        
        // 处理异步方法唤醒,如dispatch_async
        else if (wakeUpPort == mainDispatchQueuePort)
            __CFRUNLOOP_IS_SERVICING_THE_MAIN_DISPATCH_QUEUE__()
            
        // UI刷新,动画显示
        else
            __CFRunLoopDoSource1();
        
        // 再次确保是否有同步的方法需要调用
        __CFRunLoopDoBlocks();
        
    } while (!stop && !timeout);
    
    //通知即将退出runloop
    __CFRunLoopDoObservers(CFRunLoopExit);
}
不难发现NSRunLoop调用方法主要就是在kCFRunLoopBeforeSources和kCFRunLoopBeforeWaiting之间,还有kCFRunLoopAfterWaiting之后,也就是如果我们发现这两个时间内耗时太长,那么就可以判定出此时主线程卡顿.

量化卡顿的程度

要监控NSRunLoop的状态,我们需要使用到CFRunLoopObserverRef,通过它可以实时获得这些状态值的变化,具体的使用如下:


static void runLoopObserverCallBack(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    MyClass *object = (__bridge MyClass*)info;
    object->activity = activity;
}

- (void)registerObserver
{
    CFRunLoopObserverContext context = {0,(__bridge void*)self,NULL,NULL};
    CFRunLoopObserverRef observer = CFRunLoopObserverCreate(kCFAllocatorDefault,
                                                            kCFRunLoopAllActivities,
                                                            YES,
                                                            0,
                                                            &runLoopObserverCallBack,
                                                            &context);
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
}
只需要另外再开启一个线程,实时计算这两个状态区域之间的耗时是否到达某个阀值,便能揪出这些性能杀手.

为了让计算更精确,需要让子线程更及时的获知主线程NSRunLoop状态变化,所以dispatch_semaphore_t是个不错的选择,另外卡顿需要覆盖到多次连续小卡顿和单次长时间卡顿两种情景,所以判定条件也需要做适当优化.将上面两个方法添加计算的逻辑如下:


static void runLoopObserverCallBack(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
    MyClass *object = (__bridge MyClass*)info;
    
    // 记录状态值
    object->activity = activity;
    
    // 发送信号
    dispatch_semaphore_t semaphore = moniotr->semaphore;
    dispatch_semaphore_signal(semaphore);
}

- (void)registerObserver
{
    CFRunLoopObserverContext context = {0,(__bridge void*)self,NULL,NULL};
    CFRunLoopObserverRef observer = CFRunLoopObserverCreate(kCFAllocatorDefault,
                                                            kCFRunLoopAllActivities,
                                                            YES,
                                                            0,
                                                            &runLoopObserverCallBack,
                                                            &context);
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
    
    // 创建信号
    semaphore = dispatch_semaphore_create(0);
    
    // 在子线程监控时长
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        while (YES)
        {
            // 假定连续5次超时50ms认为卡顿(当然也包含了单次超时250ms)
            long st = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 50*NSEC_PER_MSEC));
            if (st != 0)
            {
                if (activity==kCFRunLoopBeforeSources || activity==kCFRunLoopAfterWaiting)
                {
                    if (++timeoutCount < 5)
                        continue;
                    
                    NSLog(@"好像有点儿卡哦");
                }
            }
            timeoutCount = 0;
        }
    });
}
记录卡顿的函数调用

监控到了卡顿现场,当然下一步便是记录此时的函数调用信息,此处可以使用一个第三方Crash收集组件PLCrashReporter,它不仅可以收集Crash信息也可用于实时获取各线程的调用堆栈,使用示例如下:

PLCrashReporterConfig *config = [[PLCrashReporterConfig alloc] initWithSignalHandlerType:PLCrashReporterSignalHandlerTypeBSD
                                                                   symbolicationStrategy:PLCrashReporterSymbolicationStrategyAll];
PLCrashReporter *crashReporter = [[PLCrashReporter alloc] initWithConfiguration:config];

NSData *data = [crashReporter generateLiveReport];
PLCrashReport *reporter = [[PLCrashReport alloc] initWithData:data error:NULL];
NSString *report = [PLCrashReportTextFormatter stringValueForCrashReport:reporter
                                                          withTextFormat:PLCrashReportTextFormatiOS];

NSLog(@"------------\n%@\n------------", report);
当检测到卡顿时,抓取堆栈信息,然后在客户端做一些过滤处理,便可以上报到服务器,通过收集一定量的卡顿数据后经过分析便能准确定位需要优化的逻辑,至此这个实时卡顿监控就大功告成了!


* out put

2016-08-23 15:11:18.318 PerformanceMonitor[3174:122076] ------------
Incident Identifier: CE20FE5C-9633-4B1B-BC82-E4249A759696
CrashReporter Key:   TODO
Hardware Model:      x86_64
Process:         PerformanceMonit [3174]
Path:            /Users/qitmac000260/Library/Developer/CoreSimulator/Devices/2E8D6495-60B7-4E94-8A2E-050A5AAA9194/data/Containers/Bundle/Application/98C5727E-6D68-4A1D-A038-A66B762C6EE6/PerformanceMonitor.app/PerformanceMonitor
Identifier:      com.tencent.PerformanceMonitor
Version:         1.0 (1)
Code Type:       X86-64
Parent Process:  debugserver [3175]

Date/Time:       2016-08-23 07:11:17 +0000
OS Version:      Mac OS X 8.3 (15G31)
Report Version:  104

Exception Type:  SIGTRAP
Exception Codes: TRAP_TRACE at 0x10346b27f
Crashed Thread:  2

Thread 0:
0   libsystem_kernel.dylib              0x000000010660c10a __semwait_signal + 10
1   libsystem_sim_c.dylib               0x00000001063cc5b8 usleep + 54
2   PerformanceMonitor                  0x000000010346ab8b -[ViewController tableView:cellForRowAtIndexPath:] + 267
3   UIKit                               0x0000000104245a28 -[UITableView _createPreparedCellForGlobalRow:withIndexPath:willDisplay:] + 508
4   UIKit                               0x0000000104224248 -[UITableView _updateVisibleCellsNow:isRecursive:] + 2853
5   UIKit                               0x000000010423a8a9 -[UITableView layoutSubviews] + 210
6   UIKit                               0x00000001041c4a2b -[UIView(CALayerDelegate) layoutSublayersOfLayer:] + 536
7   QuartzCore                          0x00000001079a0ec2 -[CALayer layoutSublayers] + 146
8   QuartzCore                          0x00000001079956d6 _ZN2CA5Layer16layout_if_neededEPNS_11TransactionE + 380
9   QuartzCore                          0x0000000107995546 _ZN2CA5Layer28layout_and_display_if_neededEPNS_11TransactionE + 24
10  QuartzCore                          0x0000000107901886 _ZN2CA7Context18commit_transactionEPNS_11TransactionE + 242
11  QuartzCore                          0x0000000107902a3a _ZN2CA11Transaction6commitEv + 462
12  QuartzCore                          0x00000001079c4075 _ZN2CA7Display11DisplayLink14dispatch_itemsEyyy + 489
13  CoreFoundation                      0x0000000103cb3174 __CFRUNLOOP_IS_CALLING_OUT_TO_A_TIMER_CALLBACK_FUNCTION__ + 20
14  CoreFoundation                      0x0000000103cb2d35 __CFRunLoopDoTimer + 1045
15  CoreFoundation                      0x0000000103c74d3d __CFRunLoopRun + 1901
16  CoreFoundation                      0x0000000103c74366 CFRunLoopRunSpecific + 470
17  GraphicsServices                    0x0000000107287a3e GSEventRunModal + 161
18  UIKit                               0x0000000104144900 UIApplicationMain + 1282
19  PerformanceMonitor                  0x000000010346b72f main + 111
20  libdyld.dylib                       0x0000000106303145 start + 1

Thread 1:
0   libsystem_kernel.dylib              0x000000010660cee2 kevent64 + 10
1   libdispatch.dylib                   0x00000001062be511 _dispatch_mgr_init + 0

Thread 2 Crashed:
0   PerformanceMonitor                  0x000000010346f285 -[PLCrashReporter generateLiveReportWithThread:error:] + 632
1   PerformanceMonitor                  0x000000010346b27f __27-[PerformanceMonitor start]_block_invoke + 447
2   libdispatch.dylib                   0x00000001062b0186 _dispatch_call_block_and_release + 12
3   libdispatch.dylib                   0x00000001062cf614 _dispatch_client_callout + 8
4   libdispatch.dylib                   0x00000001062b9552 _dispatch_root_queue_drain + 1768
5   libdispatch.dylib                   0x00000001062bab17 _dispatch_worker_thread3 + 111
6   libsystem_pthread.dylib             0x000000010663d4de _pthread_wqthread + 1129
7   libsystem_pthread.dylib             0x000000010663b341 start_wqthread + 13

Thread 3:
0   libsystem_kernel.dylib              0x000000010660c5e2 __workq_kernreturn + 10
1   libsystem_pthread.dylib             0x000000010663b341 start_wqthread + 13

Thread 2 crashed with X86-64 Thread State:
   rip: 0x000000010346f285    rbp: 0x0000700000199da0    rsp: 0x0000700000199b10    rax: 0x0000700000199b70 
   rbx: 0x0000700000199cb0    rcx: 0x0000000000003403    rdx: 0x0000000000000000    rdi: 0x000000010346f40a 
   rsi: 0x0000700000199b40     r8: 0x0000000000000014     r9: 0x0000000000000000    r10: 0x0000000106605f4e 
   r11: 0x0000000000000246    r12: 0x00007fd318d4b1e0    r13: 0x0000000000000000    r14: 0x0000000000000007 
   r15: 0x0000700000199b90 rflags: 0x0000000000000202     cs: 0x000000000000002b     fs: 0x0000000000000000 
    gs: 0x0000000000000000 

Binary Images:
       0x103469000 -        0x103491fff +PerformanceMonitor x86_64  <e50d936d109e36ecae02ab1666276144> /Users/qitmac000260/Library/Developer/CoreSimulator/Devices/2E8D6495-60B7-4E94-8A2E-050A5AAA9194/data/Containers/Bundle/Application/98C5727E-6D68-4A1D-A038-A66B762C6EE6/PerformanceMonitor.app/PerformanceMonitor
       0x1034b6000 -        0x1034dafff  dyld_sim x86_64  <c4c4d1a2e19b33bea569efbd99b392c7> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/dyld_sim
       0x103520000 -        0x103527fff  libBacktraceRecording.dylib x86_64  <c9e82206ec143591b7f48ff85358c680> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libBacktraceRecording.dylib
       0x10352d000 -        0x103532fff  libViewDebuggerSupport.dylib x86_64  <5416a438f32034eeb288b4007ac986c0> /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator9.3.sdk/Developer/Library/PrivateFrameworks/DTDDISupport.framework/libViewDebuggerSupport.dylib
       0x103539000 -        0x103806fff  Foundation x86_64  <9100ed655076380db1351d405b6e48b8> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Foundation.framework/Foundation
       0x1039df000 -        0x103bd7fff  libobjc.A.dylib x86_64  <e2d8bbf92e91324cbfb9873404db2e9a> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libobjc.A.dylib
       0x103bfb000 -        0x103c02fff  libSystem.dylib x86_64  <e20e6f02ec063a55900fb86dac634fec> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libSystem.dylib
       0x103c0a000 -        0x103faefff  CoreFoundation x86_64  <d53db557353e301487c70e129bbe917e> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
       0x104128000 -        0x104c99fff  UIKit x86_64  <e0335856510939b6a7c8cecc83482651> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/UIKit.framework/UIKit
       0x10540b000 -        0x1054e7fff  MobileCoreServices x86_64  <7ab7b0ea05ad32c2a5b6dd245276b942> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices
       0x10555e000 -        0x105581fff  libextension.dylib x86_64  <916144fee2443b288c024ef2f789e885> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libextension.dylib
       0x1055a1000 -        0x1055d3fff  libarchive.2.dylib x86_64  <e792d4788cf53e2893d3ff40f979ebf7> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libarchive.2.dylib
       0x1055dd000 -        0x105813fff  libicucore.A.dylib x86_64  <aba196e443453e63b4e36447b2a05ebf> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libicucore.A.dylib
       0x1058cd000 -        0x1059dafff  libxml2.2.dylib x86_64  <1d515ef4581331e9a2e1f9cc44f77e2a> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libxml2.2.dylib
       0x105a11000 -        0x105a23fff  libz.1.dylib x86_64  <c06afd8f746135fd94b98261cb0cfc17> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libz.1.dylib
       0x105a28000 -        0x105cbbfff  CFNetwork x86_64  <3c85232060653143afe5d550621b5cf4> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CFNetwork.framework/CFNetwork
       0x105e3f000 -        0x105ea2fff  SystemConfiguration x86_64  <9c6f5f22b9c933638f7bfaab52bd5000> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration
       0x105ed7000 -        0x105f3ffff  Security x86_64  <2bb6a432a6ce399d91b97bc0c854bde8> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Security.framework/Security
       0x105f82000 -        0x105feafff  IOKit x86_64  <12f1fd5c860e3fc3a37917b5c46b9496> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
       0x10601c000 -        0x10603dfff  libCRFSuite.dylib x86_64  <5891def8af5c3b80bf1226770f52b61c> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libCRFSuite.dylib
       0x106048000 -        0x106049fff  liblangid.dylib x86_64  <70885611710535a6af824c96a30d3548> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/liblangid.dylib
       0x10604d000 -        0x1060b8fff  libc++.1.dylib x86_64  <16bb9b750d3d3bf78108d7536137d703> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libc++.1.dylib
       0x10610d000 -        0x106128fff  libMobileGestalt.dylib x86_64  <9f5c783bb26c382887865d90fa22dbbc> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libMobileGestalt.dylib
       0x106158000 -        0x10616afff  libbsm.0.dylib x86_64  <84fe53f382ae3e0fb55faa904f75a1e9> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libbsm.0.dylib
       0x106173000 -        0x1061a0fff  libc++abi.dylib x86_64  <d43d5cb7ed0032b8af65d5e00a9fe467> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libc++abi.dylib
       0x1061ae000 -        0x1061b3fff  libcache_sim.dylib x86_64  <9bd0834165f73cec84137a61d90833b3> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libcache_sim.dylib
       0x1061b8000 -        0x1061c7fff  libcommonCrypto.dylib x86_64  <342de493a7a331c2918d966c0c3384ec> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libcommonCrypto.dylib
       0x1061d5000 -        0x1061dcfff  libcompiler_rt.dylib x86_64  <e01711e6b37f3839bf395f75967f9a27> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libcompiler_rt.dylib
       0x1061e5000 -        0x1061edfff  libcopyfile.dylib x86_64  <2dbda579f3be3e529e667c8660f3b53b> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libcopyfile.dylib
       0x1061f3000 -        0x106272fff  libcorecrypto.dylib x86_64  <d10c219429c33395936d9f524bf04276> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libcorecrypto.dylib
       0x1062ae000 -        0x1062defff  libdispatch.dylib x86_64  <a7916816457d3ba3b5adf5bbc33d9aa7> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/introspection/libdispatch.dylib
       0x106301000 -        0x106303fff  libdyld.dylib x86_64  <f4d69a4478f43035895fc767f1fc7fef> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libdyld.dylib
       0x106309000 -        0x106309fff  liblaunch.dylib x86_64  <5cc7852c28b43a25a735edd98eae74b3> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/liblaunch.dylib
       0x10630f000 -        0x106315fff  libmacho_sim.dylib x86_64  <eca6e5e8b4033e22bfce846b2e3318c2> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libmacho_sim.dylib
       0x10631b000 -        0x10631dfff  libremovefile.dylib x86_64  <1c2e631ce0b5312ab7697789e22f3be1> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libremovefile.dylib
       0x106322000 -        0x10633bfff  libsystem_asl.dylib x86_64  <e7f2f47409f93a2c88fbddccf761a018> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_asl.dylib
       0x106348000 -        0x106349fff  libsystem_sim_blocks.dylib x86_64  <8b37f561373634878e940b1670426dd7> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_blocks.dylib
       0x10634e000 -        0x1063eafff  libsystem_sim_c.dylib x86_64  <b08a213e9d293e38bf1d9c26b8239160> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_c.dylib
       0x106414000 -        0x106417fff  libsystem_sim_configuration.dylib x86_64  <f9b991bd9c3d3b2982beccda0d3c66c7> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_configuration.dylib
       0x10641d000 -        0x10641efff  libsystem_coreservices.dylib x86_64  <eec8a166feb03ab090edffa75db91bd3> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_coreservices.dylib
       0x106423000 -        0x106438fff  libsystem_coretls.dylib x86_64  <9a77131e937a3ef590994c52aa3cba19> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_coretls.dylib
       0x106449000 -        0x106452fff  libsystem_sim_dnssd.dylib x86_64  <de8451bf610c3b7e80bdc42917201adf> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_dnssd.dylib
       0x106458000 -        0x106484fff  libsystem_sim_info.dylib x86_64  <a361f51a783c373a8551878209ba8793> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_info.dylib
       0x106496000 -        0x10649afff  libsystem_sim_kernel.dylib x86_64  <d7a14fe6db173136b0e70dc822e7e405> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_kernel.dylib
       0x1064a0000 -        0x1064d1fff  libsystem_sim_m.dylib x86_64  <1032d0c44704382d813c90d6d5f81d3d> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_m.dylib
       0x1064d8000 -        0x1064f6fff  libsystem_malloc.dylib x86_64  <6e5c1529da7730e3bc6c3b18ca373e88> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_malloc.dylib
       0x1064ff000 -        0x106540fff  libsystem_network.dylib x86_64  <8f786bb592f230b1b529a60f45e23e88> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_network.dylib
       0x106565000 -        0x106570fff  libsystem_notify.dylib x86_64  <86656d9dd6b83183b9649d9ff0c69cae> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_notify.dylib
       0x106578000 -        0x10657bfff  libsystem_sim_platform.dylib x86_64  <af406e1901993838a251b0379c775a69> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_platform.dylib
       0x106580000 -        0x106581fff  libsystem_sim_pthread.dylib x86_64  <3c530e1a60a53d23bda64eb4b7c8458c> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_pthread.dylib
       0x106587000 -        0x10658afff  libsystem_sim_sandbox.dylib x86_64  <43a4cd4e88d43cc586bb7ec4708a17dc> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_sandbox.dylib
       0x106590000 -        0x106597fff  libsystem_sim_trace.dylib x86_64  <c3da37c7de073a7d98e4fb603eac04ca> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libsystem_sim_trace.dylib
       0x1065a0000 -        0x1065a6fff  libunwind_sim.dylib x86_64  <50bd947b0abe3bdfa7ece424f48d52f1> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libunwind_sim.dylib
       0x1065ad000 -        0x1065d9fff  libxpc.dylib x86_64  <23dbfc521abe3a338fe2637c3b0ea394> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/system/libxpc.dylib
       0x1065f5000 -        0x106613fff  libsystem_kernel.dylib x86_64  <c1a6a0b9186936abb4a2d862eb09a4be> /usr/lib/system/libsystem_kernel.dylib
       0x106629000 -        0x106631fff  libsystem_platform.dylib x86_64  <29a905ef67773c3382b06c3a88c4ba15> /usr/lib/system/libsystem_platform.dylib
       0x10663a000 -        0x106643fff  libsystem_pthread.dylib x86_64  <3dd1ef4c1d1b3abf8cc6b3b1ceee9559> /usr/lib/system/libsystem_pthread.dylib
       0x106651000 -        0x106749fff  libsqlite3.dylib x86_64  <a1de8eeefb3a3b52ac90fb69faa1bae3> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libsqlite3.dylib
       0x106762000 -        0x10676ffff  libbz2.1.0.dylib x86_64  <70ea3e5eca0f3bbd95303d6fb212744f> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libbz2.1.0.dylib
       0x106774000 -        0x106791fff  liblzma.5.dylib x86_64  <133480fa3a7f3fc0b5dd6ea9d757939e> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/liblzma.5.dylib
       0x106799000 -        0x10688ffff  UIFoundation x86_64  <1e76b3dabee735829a97154c311f9d61> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/UIFoundation.framework/UIFoundation
       0x106903000 -        0x10692dfff  FrontBoardServices x86_64  <fc1f215ff755349184e86db92118ae7d> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices
       0x106959000 -        0x1069a6fff  BaseBoard x86_64  <4fcafdbbc9c53abba77a645cf198676e> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/BaseBoard.framework/BaseBoard
       0x1069eb000 -        0x106a9afff  CoreUI x86_64  <f5161b3c1d043164bb9f67d72115093c> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/CoreUI.framework/CoreUI
       0x106b70000 -        0x106b8dfff  CoreVideo x86_64  <c73400d55f7d358a8246d4411a40fdc5> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreVideo.framework/CoreVideo
       0x106ba1000 -        0x106bb2fff  OpenGLES x86_64  <60808528172d38a6923cc92e180780c6> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/OpenGLES.framework/OpenGLES
       0x106bbe000 -        0x106ec3fff  VideoToolbox x86_64  <70773031dbf834dfa1f8ceccd6ba2aea> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox
       0x106f46000 -        0x106f56fff  MobileAsset x86_64  <61c5f18a12813cc59b12530ad66f9c33> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/MobileAsset.framework/MobileAsset
       0x106f65000 -        0x106f89fff  BackBoardServices x86_64  <99cc0d501a1f337bb78fa905081eccef> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices
       0x106fb0000 -        0x10712afff  CoreImage x86_64  <8217dff1feaf327fb545c5942a2f41f0> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreImage.framework/CoreImage
       0x107235000 -        0x10725cfff  DictionaryServices x86_64  <80bfbe7cfefe381fb233e779802a9c6f> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/DictionaryServices.framework/DictionaryServices
       0x10727b000 -        0x107292fff  GraphicsServices x86_64  <ce0abdaf6c8039788bdace556398b2b9> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/GraphicsServices.framework/GraphicsServices
       0x1072a9000 -        0x107474fff  CoreGraphics x86_64  <e3c5db776f483fb582564f589ecc9768> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics
       0x1074eb000 -        0x1077ecfff  ImageIO x86_64  <e2a5798f9b1433f98e3c0c8473872f80> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/ImageIO.framework/ImageIO
       0x10788e000 -        0x107a46fff  QuartzCore x86_64  <98ab28b241413d89bb48bb8fcbe497b6> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/QuartzCore.framework/QuartzCore
       0x107b01000 -        0x107b25fff  SpringBoardServices x86_64  <aefe5754d4e2373aacfc6b1a9a5e22aa> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices
       0x107b47000 -        0x107b9afff  AppSupport x86_64  <bde0d5bf587f303ab08f69c7b8c01e5e> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/AppSupport.framework/AppSupport
       0x107bd6000 -        0x107d3dfff  CoreText x86_64  <9f804cdc42e232d885c6509cace26b58> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreText.framework/CoreText
       0x107ddb000 -        0x107e11fff  TextInput x86_64  <063e4711cae3350689f2dca85b5d7ffe> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TextInput.framework/TextInput
       0x107e4c000 -        0x107f55fff  WebKitLegacy x86_64  <e475a630637f3ddc9d252fa9fab64f9e> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/WebKitLegacy.framework/WebKitLegacy
       0x108016000 -        0x1097bffff  WebCore x86_64  <19f985d1170e3dcc810d5cc6c2916b76> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/WebCore.framework/WebCore
       0x10a18a000 -        0x10a267fff  ProofReader x86_64  <cc37ec9c053c3ec894dc48947f2c1720> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/ProofReader.framework/ProofReader
       0x10a299000 -        0x10a2a6fff  libAccessibility.dylib x86_64  <33a720b917a136beb366c3770badeaa8> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libAccessibility.dylib
       0x10a2b7000 -        0x10a2b7fff  Accelerate x86_64  <51be3b0344953585a1ff610afb909d28> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Accelerate
       0x10a2ba000 -        0x10a31efff  PhysicsKit x86_64  <18d2074127f03e1db14b04ea20ecac82> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/PhysicsKit.framework/PhysicsKit
       0x10a34b000 -        0x10a34bfff  FontServices x86_64  <9a35b2a3eefb3634b611d08a5309ca16> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/FontServices.framework/FontServices
       0x10a34f000 -        0x10a449fff  libFontParser.dylib x86_64  <a8f26e7342a43b3cbe34d5d596290eca> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/FontServices.framework/libFontParser.dylib
       0x10a4ff000 -        0x10a97dfff  vImage x86_64  <f5bca4f05c3a3e539bf343a4a2a27efa> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vImage.framework/vImage
       0x10a9d7000 -        0x10a9d7fff  vecLib x86_64  <b2ef22d770843b50878fbf3ab23f0bea> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/vecLib
       0x10a9da000 -        0x10aaeefff  libvDSP.dylib x86_64  <d8031e99f6503ff28e5893e19c770c4a> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/libvDSP.dylib
       0x10aafa000 -        0x10af25fff  libLAPACK.dylib x86_64  <922d9da5a8a23ee790d97a354ba168e7> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework//libLAPACK.dylib
       0x10af59000 -        0x10b0f1fff  libBLAS.dylib x86_64  <3e15cbf768cb3277a1a2f703546a2092> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework//libBLAS.dylib
       0x10b10e000 -        0x10b1b9fff  libvMisc.dylib x86_64  <d24d01b66ab631d2902169da6596337d> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework/libvMisc.dylib
       0x10b1c2000 -        0x10b1dbfff  libLinearAlgebra.dylib x86_64  <c09601ab330b3a5a8fa2e2d558e54efd> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/Accelerate.framework/Frameworks/vecLib.framework//libLinearAlgebra.dylib
       0x10b1e3000 -        0x10b288fff  AppleJPEG x86_64  <c3ba4dd132533cf58e60b7b1a1c2fd90> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/AppleJPEG.framework/AppleJPEG
       0x10b2ee000 -        0x10b2f9fff  libGFXShared.dylib x86_64  <03f7fdff991e30a79693b2cdc71baf53> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/OpenGLES.framework/libGFXShared.dylib
       0x10b300000 -        0x10b348fff  libGLImage.dylib x86_64  <81cfe3e4bfe73fbda43b1eb4393f02c5> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/OpenGLES.framework/libGLImage.dylib
       0x10b351000 -        0x10b353fff  libCVMSPluginSupport.dylib x86_64  <522ee7aa8db13f04bff871c3eee986b0> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/OpenGLES.framework/libCVMSPluginSupport.dylib
       0x10b359000 -        0x10b365fff  libCoreVMClient.dylib x86_64  <7055bdffafea35baacabe8e47529ab3e> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/OpenGLES.framework/libCoreVMClient.dylib
       0x10b36f000 -        0x10c0fefff  libLLVMContainer.dylib x86_64  <a45c73b56bbc330a9e52df1bc2bc2ff4> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/OpenGLES.framework//libLLVMContainer.dylib
       0x10c44a000 -        0x10c459fff  AssertionServices x86_64  <30e9ae5b4ac83de3aa38fcd571dd9b42> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/AssertionServices.framework/AssertionServices
       0x10c46c000 -        0x10c8aefff  FaceCore x86_64  <9fbefd9fab043d87a795a722a689e510> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/FaceCore.framework/FaceCore
       0x10cac6000 -        0x10cc3bfff  libFosl_dynamic.dylib x86_64  <fe10eb4e331a3e649691747c08eda021> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libFosl_dynamic.dylib
       0x10ccbd000 -        0x10cd2ffff  CoreMedia x86_64  <e74e08b2871e3fd1b12e37baf6ba04ea> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreMedia.framework/CoreMedia
       0x10cd87000 -        0x10cd88fff  SimulatorClient x86_64  <163df00d4c4d3076b84bad1aa0482861> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/SimulatorClient.framework/SimulatorClient
       0x10cd8d000 -        0x10cde8fff  CoreAudio x86_64  <e5716f7398f43692be30e0ae379316f5> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreAudio.framework/CoreAudio
       0x10ce0c000 -        0x10ce39fff  libxslt.1.dylib x86_64  <4fe91c9a8b89378c871573ad0a0bd066> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libxslt.1.dylib
       0x10ce46000 -        0x10d464fff  JavaScriptCore x86_64  <04ab8e88e2153d5e996db0c20d487324> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/JavaScriptCore.framework/JavaScriptCore
       0x10d5e6000 -        0x10d8ccfff  AudioToolbox x86_64  <1d08769644493117a821685e517f70c6> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox
       0x10d9c3000 -        0x10d9c8fff  TCC x86_64  <0da785f46119380197481b6bae18bc77> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/TCC.framework/TCC
       0x10d9cf000 -        0x10da1ffff  LanguageModeling x86_64  <e70649a347ef30a380f7eb2abefa0c99> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/LanguageModeling.framework/LanguageModeling
       0x10da30000 -        0x10da35fff  AggregateDictionary x86_64  <5e75f76e4faa3d299a12c9082000100a> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/AggregateDictionary.framework/AggregateDictionary
       0x10da3c000 -        0x10da50fff  libcmph.dylib x86_64  <b1581c7ea5dd3954aa822cf1b5379884> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/usr/lib/libcmph.dylib
       0x10da5f000 -        0x10da67fff  MediaAccessibility x86_64  <5c844e24b602311b8862b1e42e418625> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/MediaAccessibility.framework/MediaAccessibility
       0x10dd80000 -        0x10dd86fff  ConstantClasses x86_64  <87731ea65e59308e91f07ce3574eb2ba> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/ConstantClasses.framework/ConstantClasses
       0x10dd8f000 -        0x10dd92fff  libCGXType.A.dylib x86_64  <4ee373c74a023c42ae2db31265a4c327> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreGraphics.framework/Resources/libCGXType.A.dylib
       0x10ddaf000 -        0x10ddd8fff  libRIP.A.dylib x86_64  <c9e74f2ce6f73690b92778f6cfaa28e9> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreGraphics.framework/Resources/libRIP.A.dylib
       0x10dde7000 -        0x10ddf6fff  libCMSBuiltin.A.dylib x86_64  <25cc2204b4223d1dbbaf7e8511d4984a> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks/CoreGraphics.framework/Resources/libCMSBuiltin.A.dylib
       0x11063f000 -        0x11064efff  libGSFontCache.dylib x86_64  <2e37accdfd9f3dd8985336bfedf84e4f> /Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 8.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/PrivateFrameworks/FontServices.framework/libGSFontCache.dylib

------------

