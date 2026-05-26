data_collect/
├── CMakeLists.txt                # 顶层 CMake 构建脚本
├── README.md                     # 项目说明、引脚映射与运行指令
├── lib                           # OAK 原操作函数依赖库（.so .a）
├── scripts/                      # 部署与系统配置脚本
│   ├── build_cross.sh            # 交叉编译
│   ├── manage.sh                 # 程序运行管理
│   ├── sync_sysroot.sh           # 同步sysroot依赖
│   └── setup_env.sh              # 环境初始化脚本 (如 IRQ 绑定等)
├── include/                      # 头文件目录 (对外接口定义)
│   ├── core/
│   │   ├── concurrentqueue.h     # 强烈建议引入 moodycamel 的无锁队列实现
│   │   ├── encode_worker.hpp     # 定义 视频 raw -> H264 处理工具类 
│   │   ├── data_packet.hpp       # 定义 DataPacket 结构体 (sys_ts, dev_ts, payload)
│   │   └── data_bridge.hpp       # 定义无锁环形缓冲区接口
│   ├── providers/
│   │   ├── carina_a1088.h        # OAK 原操作函数控制类 
│   │   ├── oak_provider.hpp      # OAK 控制类 
│   │   └── dv89_provider.hpp     # DV89 控制类 (V4L2 节点, M2M 硬件编码)
│   ├── storage/
│   │   └── storage_worker.hpp    # 异步落盘线程类
│   └── utils/
│       ├── time_sync.hpp         # 时间戳获取工具 (封装 CLOCK_MONOTONIC_RAW)
│       └── logger.hpp            # 终端日志打印工具
└── src/                          # 源文件目录 (业务逻辑实现)
    ├── main.cpp                  # 程序入口 (解析参数、初始化硬件、拉起所有线程)
    ├── core/
    │   ├── encode_worker.cpp     
    │   └── data_bridge.cpp       
    ├── providers/
    │   ├── oak_provider.cpp      
    │   └── dv89_provider.cpp     
    └── storage/
        └── storage_worker.cpp


1 环境配置部署下载（在板子上执行）
    插上 SD 卡和所有摄像头，运行自动化脚本：
    cd scripts/
    sudo ./setup_env.sh
    脚本运行后会自动将 SD 卡挂载到 /sdcard，并锁定 CPU 为性能模式。

2 同步板子的依赖库（虚拟机）
    sudo ./sync_sysroot.sh

3 交叉编译（虚拟机）
    sudo ./build_cross.sh

4 将生成的可执行文件和manage.sh 传到板子上
    启动 (带参)	    开始采集，自定义目录名	            ./scripts/manage.sh start tunnel_test
    启动 (不带参)   开始采集，以 UNIX 时间戳命名	    ./scripts/manage.sh start
    查看状态        检查进程是否运行及实时日志	        ./scripts/manage.sh status
    停止采样	    安全停止，触发落盘扫尾	            ./scripts/manage.sh stop

5 实时查看系统运行状态：
    tail -f /tmp/vio_sys.log


6 采集完成后，数据将保存在 /sdcard/data_XXXXXXXX/ 目录下：
    .
    ├── oak_stereo.h264   # OAK-D 双目硬件编码视频
    ├── dv89_aux.h264     # DV89 辅助摄像头编码视频
    ├── imu_raw.bin       # 原始 IMU 数据 (Binary 格式)
    └── sync_map.log      # 时间戳映射表 (用于离线对齐)
    sync_map.log格式说明，每行记录如下： [系统时间戳(ns)], [硬件时间戳(ns)], [数据类型ID], [数据大小(bytes)]