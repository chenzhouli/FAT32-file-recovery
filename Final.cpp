#include <windows.h>
#include <stdio.h>
#include <winioctl.h>
#include <string.h>
#pragma pack(push, 1)

//DBR/引导扇区
typedef struct {
    BYTE jmp[3]; //跳转指令
    BYTE oem[8]; //文件系统标志和版本号
    WORD bytesPerSector; //每扇区字节数
    BYTE sectorsPerCluster; //每簇扇区数
    WORD reservedSectorCount; //保留扇区数
    BYTE numberOfFATs;  //FAT个数
    WORD rootEntryCount;  //根目录最多容纳目录项数（0）
    WORD totalSectors16;  //扇区总数（0）
    BYTE mediaType;  //介质描述符
    WORD sectorsPerFAT16;  //每FAT的扇区数（0）
    WORD sectorsPerTrack;  //每磁道扇区数
    WORD numberOfHeads;  //磁头数
    DWORD hiddenSectors;  //分区前已使用扇区数/相对于MBR的地址偏移量
    DWORD totalSectors32;  //总扇区数 
    DWORD sectorsPerFAT32;  //每个FAT表的扇区数
    WORD extFlags;  //标记
    WORD fsVersion;  //版本号
    DWORD rootClusterNumber;  //根目录起始簇号
    WORD fsInfoSectorNumber;  //FSINFO扇区号
    WORD backupBootSectorNumber;  //备份引导扇区号
    BYTE reserved[12];  //未使用
    BYTE driveNumber;   //设备号
    BYTE reserved1;  //未使用
    BYTE bootSignature;  //扩展引导标志
    DWORD volumeID;  //卷序列号
    BYTE volumeLabel[11];  //卷标
    BYTE fileSystemType[8];  //文件系统格式的ASCII码
    //剩余一些未使用空间未定义
} BPB;

//目录项
typedef struct {
    BYTE name[11];  //00为分配状态，0xE5代表删除；文件名
    BYTE attributes;  //文件属性
    BYTE reserved;  //未使用
    BYTE creationTimeTenth; //建立时间
    WORD creationTime; //建立时间
    WORD creationDate; //建立日期
    WORD lastAccessDate;  //最后访问日期
    WORD firstClusterHigh;  //起始簇号的高字节
    WORD writeTime; //最后修改时间
    WORD writeDate; //最后修改日期
    WORD firstClusterLow;  //起始簇号低字节
    DWORD fileSize;  //内容大小
} DirEntry;

#pragma pack(pop)

#define DIR_ENTRY_SIZE 32  //目录项长度
#define DBR_SIZE 512  //引导扇区长度

//按8.3格式修改名称
void formatFilename(char* dest, const char* src) {
    // Convert to uppercase and add spaces
    int i;
    for (i = 0; i < 8; i++) {
        if (*src != '.' && *src != '\0') {
            *dest++ = toupper(*src++);
        }
        else {
            *dest++ = ' ';
        }
    }
    if (*src == '.') src++;
    for (i = 0; i < 3; i++) {
        if (*src != '\0') {
            *dest++ = toupper(*src++);
        }
        else {
            *dest++ = ' ';
        }
    }
}

//判断是否以管理员身份运行
BOOL IsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;

    //创建一个 well-known SID (安全标识符) 代表本地管理员组。
    SID_IDENTIFIER_AUTHORITY ntAuthority = { SECURITY_NT_AUTHORITY };
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
    }

    if (adminGroup) {
        FreeSid(adminGroup);
    }

    return isAdmin;
}


int main() {
    if (!IsAdmin()) {
        printf("该程序需要以管理员权限运行。\n");
        return 1;
    }

    char path[256];  //需要恢复文件的路径，支持最多256字符
    printf("请输入路径：");
    fgets(path, sizeof(path), stdin);

    //删除换行符，fgets会将其一并读入
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '\n') {
        path[len - 1] = '\0';
    }

    char* directory;
    directory = strtok(path, "/");  //将'E:'拆分出来
    directory = strtok(NULL, "/");  //再次调用strtok，获取路径的第一个子目录

    const char* device = "\\\\.\\PHYSICALDRIVE1";  //扫描的设备名（U盘）
    //const char* filename = "test.jpg";  //要恢复的文件名，8.3格式

    //创建文件
    HANDLE hDevice = CreateFile(device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("Error opening device %s\n", device);
        return 1;
    }

    //锁定卷
    DWORD bytesReturned;
    if (!DeviceIoControl(hDevice, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL)){
        printf("DeviceIoControl (FSCTL_LOCK_VOLUME) failed with error %lu\n", GetLastError());
        CloseHandle(hDevice);
        return 1;
    }

    //物理硬盘中有未使用分区在E:\分区前面，故计算地址时需加上
    LONG offset = 4128768;
    if (SetFilePointer(hDevice, offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        printf("Error");
        CloseHandle(hDevice);
        return 1;
    }

    BYTE* buffer = NULL; //读取缓冲区
    DWORD bytesRead;
    DWORD bytesWritten;
    BPB bpb;  //存储引导扇区相关信息

    //读取引导扇区
    buffer = (BYTE*)malloc(DBR_SIZE);
    if (!buffer) {
        printf("Error allocating memory\n");
        CloseHandle(hDevice);
        return 1;
    }

    if (!ReadFile(hDevice, buffer, DBR_SIZE, &bytesRead, NULL)) {
        printf("Error reading device %s. Error code: %d\n", device, GetLastError());
        CloseHandle(hDevice);
        return 1;
    }

    //从缓冲区复制需要的数据到bpb
    memcpy(&bpb, buffer, sizeof(bpb));

    //从根目录开始读取目录（整个簇）
    buffer = (BYTE*)malloc((LONGLONG)bpb.sectorsPerCluster * bpb.bytesPerSector);
    if (!buffer) {
        printf("Error allocating memory\n");
        CloseHandle(hDevice);
        return 1;
    }
    
    int i;
    DirEntry* dirEntry;
    LARGE_INTEGER rootOffset;  //目录绝对位置
    DWORD beginCluster = bpb.rootClusterNumber;//簇号
    char* nextDirectory = strtok(NULL, "/");//路径中下一个子目录
    while (directory != NULL) {
        //计算簇号对应的偏移地址
        rootOffset.QuadPart = ((LONGLONG)bpb.reservedSectorCount + (LONGLONG)bpb.numberOfFATs * bpb.sectorsPerFAT32 + (beginCluster - 2) * (LONGLONG)bpb.sectorsPerCluster) * bpb.bytesPerSector + offset;
        if (!SetFilePointerEx(hDevice, rootOffset, NULL, FILE_BEGIN)) { //使指针移动到指定位置
            printf("Error seeking device %s\n", device);
            CloseHandle(hDevice);
            return 1;
        }

        if (!ReadFile(hDevice, buffer, bpb.sectorsPerCluster * bpb.bytesPerSector, &bytesRead, NULL)) {
            printf("Error reading device %s. Error code: %d\n", device, GetLastError());
            CloseHandle(hDevice);
            return 1;
        }

        if (nextDirectory == NULL) {
            //在最后一个目录查找文件
            char formatname[12]; //8.3格式
            formatFilename(formatname, directory);//格式化名称

            //遍历该簇，寻找对应的目录项
            for (i = 0; i < bpb.sectorsPerCluster * bpb.bytesPerSector; i += DIR_ENTRY_SIZE) {
                dirEntry = (DirEntry*)(buffer + i);//转为DirEntry结构体

                //检查该目录项是否是被删除（名称第1个字符是否为0xE5）的且指定的文件
                if (dirEntry->name[0] == 0xE5 && memcmp(dirEntry->name + 1, formatname + 1, 10) == 0) {
                    printf("Deleted file %s found at cluster %d, %d row\n", directory, beginCluster, i/ DIR_ENTRY_SIZE);

                    //恢复名称第1个字符
                    memcpy(buffer + i, &formatname[0], sizeof(BYTE));

                    //将该簇写回磁盘
                    if (!SetFilePointerEx(hDevice, rootOffset, NULL, FILE_BEGIN)) {
                        printf("Error seeking device %s\n", device);
                        CloseHandle(hDevice);
                        return 1;
                    }
                    if (!WriteFile(hDevice, buffer, bpb.sectorsPerCluster * bpb.bytesPerSector, &bytesWritten, NULL)) {
                        printf("Error writing FAT entry, code:%d\n", GetLastError());
                        CloseHandle(hDevice);
                        exit(1);
                    }

                    //从目录项获得该文件的起始簇号
                    DWORD startCluster = dirEntry->firstClusterLow;
                    BYTE jpegStart[] = { 0xFF, 0xD8 };
                    BYTE jpegEnd[] = { 0xFF, 0xD9 };

                    //读取起始簇（整个簇）
                    LARGE_INTEGER clusterOffset;
                    clusterOffset.QuadPart = ((LONGLONG)bpb.reservedSectorCount + (LONGLONG)bpb.numberOfFATs * bpb.sectorsPerFAT32 + (startCluster - 2) * (LONGLONG)bpb.sectorsPerCluster) * bpb.bytesPerSector + offset;
                    if (!SetFilePointerEx(hDevice, clusterOffset, NULL, FILE_BEGIN)) {
                        printf("Error seeking device %s\n", device);
                        CloseHandle(hDevice);
                        return 1;
                    }
                    if (!ReadFile(hDevice, buffer, bpb.sectorsPerCluster * bpb.bytesPerSector, &bytesRead, NULL)) {
                        printf("Error reading device %s\n", device);
                        CloseHandle(hDevice);
                        return 1;
                    }

                    //确认该簇是否为JPEG的开始部分
                    if (memcmp(buffer, jpegStart, sizeof(jpegStart)) != 0) {
                        printf("Error: cluster does not start with JPEG start sequence\n");
                        return 1;
                    }
                    else {
                        printf("Found JPEG begin sequence at cluster %d\n", startCluster);
                    }

                    //读取FAT1表（该扇区）
                    LARGE_INTEGER fatOffset;
                    BYTE* buffer2 = NULL;
                    buffer2 = (BYTE*)malloc(bpb.bytesPerSector);
                    if (!buffer2) {
                        printf("Error allocating memory\n");
                        CloseHandle(hDevice);
                    }

                    //FAT1表开始的位置
                    fatOffset.QuadPart = (LONGLONG)bpb.reservedSectorCount * bpb.bytesPerSector + offset;
                    if (!SetFilePointerEx(hDevice, fatOffset, NULL, FILE_BEGIN)) {
                        printf("Error seeking device\n");
                        CloseHandle(hDevice);
                        exit(1);
                    }
                    if (!ReadFile(hDevice, buffer2, bpb.bytesPerSector, &bytesRead, NULL)) {
                        printf("Error reading device, code:%d\n", GetLastError());
                        CloseHandle(hDevice);
                        exit(1);
                    }

                    DWORD currentCluster = startCluster;
                    DWORD nextCluster = currentCluster + 1;
                    bool ifend = false;//判断是否找到文件结尾所在的簇

                    //遍历并检查所有连续的簇
                    while (true) {
                        //读取当前簇
                        clusterOffset.QuadPart = ((LONGLONG)bpb.reservedSectorCount + (LONGLONG)bpb.numberOfFATs * bpb.sectorsPerFAT32 + (nextCluster - 2) * (LONGLONG)bpb.sectorsPerCluster) * bpb.bytesPerSector + offset;
                        if (!SetFilePointerEx(hDevice, clusterOffset, NULL, FILE_BEGIN)) {
                            printf("Error seeking device %s\n", device);
                            CloseHandle(hDevice);
                            return 1;
                        }
                        if (!ReadFile(hDevice, buffer, bpb.sectorsPerCluster * bpb.bytesPerSector, &bytesRead, NULL)) {
                            printf("Error reading device %s\n", device);
                            CloseHandle(hDevice);
                            return 1;
                        }

                        //遍历该簇，判断是否为JPEG的结束部分
                        for (int i = 0; i < bpb.sectorsPerCluster * bpb.bytesPerSector - sizeof(jpegEnd) + 1; i++) {
                            if (memcmp(buffer + i, jpegEnd, sizeof(jpegEnd)) == 0) {
                                printf("Found JPEG end sequence at cluster %d\n", nextCluster);
                                ifend = true;
                                break;
                            }
                        }

                        //为当前簇在FAT1表中创建一个条目，使其指向下一个簇
                        memcpy(buffer2 + (currentCluster * 4), &nextCluster, sizeof(DWORD));

                        //更新
                        currentCluster = nextCluster;
                        nextCluster++;

                        //完整查找后跳出循环
                        if (ifend) { break; }
                    }
                    //为最后一个簇在FAT中创建一个条目，表示这是最后一个簇
                    nextCluster = 0x0FFFFFFF;
                    memcpy(buffer2 + (currentCluster * 4), &nextCluster, sizeof(DWORD));

                    //将整个扇区（FAT1表）写入磁盘
                    if (!SetFilePointerEx(hDevice, fatOffset, NULL, FILE_BEGIN)) {
                        printf("Error seeking device\n");
                        CloseHandle(hDevice);
                        exit(1);
                    }
                    if (!WriteFile(hDevice, buffer2, bpb.bytesPerSector, &bytesWritten, NULL)) {
                        printf("Error writing FAT entry, code:%d\n", GetLastError());
                        CloseHandle(hDevice);
                        exit(1);
                    }
                    free(buffer2);
                    break;
                }
            }
            //未找到
            if (i >= bpb.sectorsPerCluster * bpb.bytesPerSector) {
                printf("Cannot find the file!");
                CloseHandle(hDevice);
                return 1;
            }
        }
        else {
        //查找子目录
            //将子目录转为大写，与存储的数据匹配
            for (int i = 0; directory[i]; i++) {
                directory[i] = toupper((unsigned char)directory[i]);
            }

            //遍历整个簇，寻找对应的目录项
            for (i = 0; i < bpb.sectorsPerCluster * bpb.bytesPerSector; i += DIR_ENTRY_SIZE) {
                dirEntry = (DirEntry*)(buffer + i);//转为DirEntry结构体

                //判断该目录项是否为目录属性且是否为指定的
                if (memcmp(dirEntry->name, directory, strlen(directory)) == 0 && dirEntry->attributes == 0x10) {
                    beginCluster = ((DWORD)dirEntry->firstClusterHigh << 16) | dirEntry->firstClusterLow;
                    break;
                }
            }
            //未找到
            if (i >= bpb.sectorsPerCluster * bpb.bytesPerSector) {
                printf("Cannot find the file!");
                CloseHandle(hDevice);
                return 1;
            }
        }
        directory = nextDirectory;
        nextDirectory = strtok(NULL, "/");
    }

    //解锁卷
    if (!DeviceIoControl(hDevice, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL)){
        printf("DeviceIoControl (FSCTL_UNLOCK_VOLUME) failed with error %lu\n", GetLastError());
        CloseHandle(hDevice);
        return 1;
    }

    printf("Successfully restored the jpg file\n");
    free(buffer);
    CloseHandle(hDevice);
    return 0;
}