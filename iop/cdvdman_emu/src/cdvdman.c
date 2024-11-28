/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "internal.h"
#include "xmodload.h"

IRX_ID(MODNAME, 1, 1);

//------------------ Patch Zone ----------------------
struct cdvdman_settings_common cdvdman_settings = {MODULE_SETTINGS_MAGIC};

//----------------------------------------------------
extern struct irx_export_table _exp_cdvdman;
extern struct irx_export_table _exp_cdvdstm;

// internal functions prototypes
static int cdvdman_writeSCmd(u8 cmd, const void *in, u16 in_size, void *out, u16 out_size);
static unsigned int event_alarm_cb(void *args);
static void cdvdman_startThreads(void);
static void cdvdman_create_semaphores(void);
static int cdvdman_read(u32 lsn, u32 sectors, u16 sector_size, void *buf);

struct cdvdman_cb_data
{
    void (*user_cb)(int reason);
    int reason;
};

cdvdman_status_t cdvdman_stat;
static struct cdvdman_cb_data cb_data;

int cdrom_io_sema;
static int cdrom_rthread_sema;
static int cdvdman_scmdsema;
int cdvdman_searchfilesema;
static int cdvdman_ReadingThreadID;

static StmCallback_t Stm0Callback = NULL;
static iop_sys_clock_t gCallbackSysClock;

// buffers
u8 cdvdman_buf[CDVDMAN_BUF_SECTORS * 2048];
u8 *cdvdman_fs_buf;

#define CDVDMAN_MODULE_VERSION 0x225
static int cdvdman_debug_print_flag = 0;

volatile unsigned char sync_flag_locked;
volatile unsigned char cdvdman_cdinited = 0;
static unsigned int ReadPos = 0; /* Current buffer offset in 2048-byte sectors. */

//-------------------------------------------------------------------------
void cdvdman_init(void)
{
    if (!cdvdman_cdinited) {
        cdvdman_stat.err = SCECdErNO;

        cdvdman_fs_init();
        cdvdman_cdinited = 1;
    }
}

int sceCdInit(int init_mode)
{
    M_DEBUG("%s(%d)\n", __FUNCTION__, init_mode);

    cdvdman_init();
    return 1;
}

//-------------------------------------------------------------------------
static unsigned int cdvdemu_read_end_cb(void *arg)
{
    iSetEventFlag(cdvdman_stat.intr_ef, CDVDEF_READ_END);
    return 0;
}

static int cdvdman_read_sectors(u32 lsn, unsigned int sectors, void *buf)
{
    unsigned int remaining;
    void *ptr;
    int endOfMedia = 0;
    u32 usec_per_sector = 0;

    //M_DEBUG("cdvdman_read_sectors lsn=%lu sectors=%u buf=%p\n", lsn, sectors, buf);

    if ((cdvdman_settings.flags & CDVDMAN_COMPAT_FAST_READS) == 0) {
        /*
         * Limit transfer speed to match the physical drive in the ps2
         *
         * Base read speeds:
         * -  1x =  150KiB/s =   75 sectors/s for CD
         * -  1x = 1350KiB/s =  675 sectors/s for DVD
         *
         * Maximum read speeds:
         * - 24x = 3600KiB/s = 1900 sectors/s for CD
         * -  4x = 5400KiB/s = 2700 sectors/s for DVD
         *
         * CLV read speed is constant (Maximum / 2.4):
         * - 10.00x = 1500KiB/s for CD
         * -  1.67x = 2250KiB/s for DVD
         *
         * CAV read speed is:
         * - Same as CLV at the inner sectors
         * - Same as max at the outer sectors
         *
         * Sony documentation states only CAV is used.
         * But there is some discussion about if this is true 100% of the time.
         */

        if (cdvdman_settings.media == 0x12) {
            // CD constant values
            // ------------------
            // 2 KiB
            // 1000000 us / s
            // 333000 sectors per CD
            // 1500 KiB/s inner speed (10X)
            // 3600 KiB/s outer speed (24X)
            const u32 cd_const_1 = (1000000 * 2 * 333000ll) / (3600 - 1500);
            const u32 cd_const_2 = (       1500 * 333000ll) / (3600 - 1500);
            usec_per_sector = cd_const_1 / (cd_const_2 + lsn);
            // CD is limited to 3000KiB/s = 667ms
            // Compensation: our code seems 23ms / sector slower than sony CDVD
            if (usec_per_sector < (667-23))
                usec_per_sector = (667-23);
        } else if (cdvdman_settings.layer1_start != 0) {
            // DVD dual layer constant values
            // ------------------------------
            // 2 KiB
            // 1000000 us / s
            // 2084960 sectors per DVD (8.5GB/2)
            // 2250 KiB/s inner speed (1.67X)
            // 5400 KiB/s outer speed (4X)
            const u32 dvd_dl_const_1 = (1000000 * 2 * 2084960ll) / (5400 - 2250);
            const u32 dvd_dl_const_2 = (       2250 * 2084960ll) / (5400 - 2250);
            // For dual layer DVD, the second layer starts at 0
            // PS2 uses PTP = Parallel Track Path
            u32 effective_lsn = lsn;
            if (effective_lsn >= cdvdman_settings.layer1_start)
                effective_lsn -= cdvdman_settings.layer1_start;

            usec_per_sector = dvd_dl_const_1 / (dvd_dl_const_2 + effective_lsn);
        }
        else {
            // DVD single layer constant values
            // --------------------------------
            // 2 KiB
            // 1000000 us / s
            // 2298496 sectors per DVD (4.7GB)
            // 2250 KiB/s inner speed (1.67X)
            // 5400 KiB/s outer speed (4X)
            const u32 dvd_sl_const_1 = (1000000 * 2 * 2298496ll) / (5400 - 2250);
            const u32 dvd_sl_const_2 = (       2250 * 2298496ll) / (5400 - 2250);
            usec_per_sector = dvd_sl_const_1 / (dvd_sl_const_2 + lsn);
        }
        // Compensation: our code seems 55ms / sector slower than sony CDVD
        usec_per_sector -= 30;

        M_DEBUG("Sector %lu (%u sectors) CAV usec_per_sector = %d\n", lsn, sectors, usec_per_sector);
    }

    if (mediaLsnCount) {

        // If lsn to read is already bigger error already.
        if (lsn >= mediaLsnCount) {
            M_DEBUG("cdvdman_read eom lsn=%d sectors=%d leftsectors=%d MaxLsn=%d \n", lsn, sectors, mediaLsnCount - lsn, mediaLsnCount);
            cdvdman_stat.err = SCECdErIPI;
            return 1;
        }

        // As per PS2 mecha code continue to read what you can and then signal end of media error.
        if ((lsn + sectors) > mediaLsnCount) {
            M_DEBUG("cdvdman_read eom lsn=%d sectors=%d leftsectors=%d MaxLsn=%d \n", lsn, sectors, mediaLsnCount - lsn, mediaLsnCount);
            endOfMedia = 1;
            // Limit how much sectors we can read.
            sectors = mediaLsnCount - lsn;
        }
    }

    cdvdman_stat.err = SCECdErNO;
    for (ptr = buf, remaining = sectors; remaining > 0;) {
        unsigned int SectorsToRead = remaining;

        if ((cdvdman_settings.flags & CDVDMAN_COMPAT_FAST_READS) == 0) {
            // Limit transfers to a maximum length of 8, with a restricted transfer rate.
            iop_sys_clock_t TargetTime;

            if (SectorsToRead > 8)
                SectorsToRead = 8;

            USec2SysClock(usec_per_sector * SectorsToRead, &TargetTime);
            ClearEventFlag(cdvdman_stat.intr_ef, ~CDVDEF_READ_END);
            SetAlarm(&TargetTime, &cdvdemu_read_end_cb, NULL);
        }

        cdvdman_stat.err = DeviceReadSectors(lsn, ptr, SectorsToRead);
        if (cdvdman_stat.err != SCECdErNO) {
            if ((cdvdman_settings.flags & CDVDMAN_COMPAT_FAST_READS) == 0)
                CancelAlarm(&cdvdemu_read_end_cb, NULL);
            break;
        }

        /* PS2LOGO Decryptor algorithm; based on misfire's code (https://github.com/mlafeldt/ps2logo)
           The PS2 logo is stored within the first 12 sectors, scrambled.
           This algorithm exploits the characteristic that the value used for scrambling will be recorded,
           when it is XOR'ed against a black pixel. The first pixel is black, hence the value of the first byte
           was the value used for scrambling. */
        if (lsn < 13) {
            u32 j;
            u8 *logo = (u8 *)ptr;
            static u8 key = 0;
            if (lsn == 0) // First sector? Copy the first byte as the value for unscrambling the logo.
                key = logo[0];
            if (key != 0) {
                for (j = 0; j < (SectorsToRead * 2048); j++) {
                    logo[j] ^= key;
                    logo[j] = (logo[j] << 3) | (logo[j] >> 5);
                }
            }
        }

        ptr = (void *)((u8 *)ptr + (SectorsToRead * 2048));
        remaining -= SectorsToRead;
        lsn += SectorsToRead;
        ReadPos += SectorsToRead * 2048;

        if ((cdvdman_settings.flags & CDVDMAN_COMPAT_FAST_READS) == 0) {
            // Sleep until the required amount of time has been spent.
            WaitEventFlag(cdvdman_stat.intr_ef, CDVDEF_READ_END, WEF_AND, NULL);
        }
    }

    // If we had a read that went past the end of media, after reading what we can, set the end of media error.
    if (endOfMedia) {
        cdvdman_stat.err = SCECdErEOM;
    }

    return (cdvdman_stat.err == SCECdErNO ? 0 : 1);
}

static int cdvdman_read(u32 lsn, u32 sectors, u16 sector_size, void *buf)
{
    cdvdman_stat.status = SCECdStatRead;

    // OPL only has 2048 bytes no matter what. For other sizes we have to copy to the offset and prepoluate the sector header data (the extra bytes.)
    u32 offset = 0;

    if (sector_size == 2340)
        offset = 12; // head - sub - data(2048) -- edc-ecc

    buf = (void *)PHYSADDR(buf);
    if (((u32)(buf)&3) || (sector_size != 2048)) {
        // For transfers to unaligned buffers, a double-copy is required to avoid stalling the device's DMA channel.
        WaitSema(cdvdman_searchfilesema);

        u32 nsectors, nbytes;
        u32 rpos = lsn;

        while (sectors > 0) {
            nsectors = sectors;
            if (nsectors > CDVDMAN_BUF_SECTORS)
                nsectors = CDVDMAN_BUF_SECTORS;

            // For other sizes we can only read one sector at a time.
            // There are only very few games (CDDA games, EA Tiburon) that will be affected
            if (sector_size != 2048)
                nsectors = 1;

            cdvdman_read_sectors(rpos, nsectors, cdvdman_buf);

            rpos += nsectors;
            sectors -= nsectors;
            nbytes = nsectors * sector_size;


            // Copy the data for buffer.
            // For any sector other than 2048 one sector at a time is copied.
            memcpy((void *)((u32)buf + offset), cdvdman_buf, nbytes);

            // For these custom sizes we need to manually fix the header.
            // For 2340 we have 12bytes. 4 are position.
            if (sector_size == 2340) {
                u8 *header = (u8 *)buf;
                // position.
                sceCdlLOCCD p;
                sceCdIntToPos(rpos - 1, &p); // to get current pos.
                header[0] = p.minute;
                header[1] = p.second;
                header[2] = p.sector;
                header[3] = 0; // p.track for cdda only non-zero

                // Subheader and copy of subheader.
                header[4] = header[8] = 0;
                header[5] = header[9] = 0;
                header[6] = header[10] = 0x8;
                header[7] = header[11] = 0;
            }

            buf = (void *)((u8 *)buf + nbytes);
        }

        SignalSema(cdvdman_searchfilesema);
    } else {
        cdvdman_read_sectors(lsn, sectors, buf);
    }

    ReadPos = 0; /* Reset the buffer offset indicator. */

    cdvdman_stat.status = SCECdStatPause;

    return 1;
}

//-------------------------------------------------------------------------
u32 sceCdGetReadPos(void)
{
    M_DEBUG("%s() = %d\n", __FUNCTION__, ReadPos);

    return ReadPos;
}

int sceCdRead_internal(u32 lsn, u32 sectors, void *buf, sceCdRMode *mode, enum ECallSource source)
{
    static u32 free_prev = 0;
    u32 free;
    int IsIntrContext, OldState;
    u16 sector_size = 2048;

    IsIntrContext = QueryIntrContext();

    if (mode != NULL)
        M_DEBUG("%s(%d, %d, %08x, {%d, %d, %d}, %d) ic=%d\n", __FUNCTION__, (int)lsn, (int)sectors, (int)buf, mode->trycount, mode->spindlctrl, mode->datapattern, (int)source, IsIntrContext);
    else
        M_DEBUG("%s(%d, %d, %08x, NULL, %d) ic=%d\n", __FUNCTION__, (int)lsn, (int)sectors, (int)buf, (int)source, IsIntrContext);

    // Is is NULL in our emulated cdvdman routines so check if valid.
    if (mode) {
        // 0 is 2048
        if (mode->datapattern == SCECdSecS2328)
            sector_size = 2328;

        if (mode->datapattern == SCECdSecS2340)
            sector_size = 2340;
    }

    free = QueryTotalFreeMemSize();
    if (free != free_prev) {
        free_prev = free;
        M_PRINTF("- memory free = %dKiB\n", free / 1024);
    }

    CpuSuspendIntr(&OldState);

    if (sync_flag_locked) {
        CpuResumeIntr(OldState);
        M_DEBUG("%s: exiting (sync_flag_locked)...\n", __FUNCTION__);
        return 0;
    }

    if (IsIntrContext)
        iClearEventFlag(cdvdman_stat.intr_ef, ~CDVDEF_MAN_UNLOCKED);
    else
        ClearEventFlag(cdvdman_stat.intr_ef, ~CDVDEF_MAN_UNLOCKED);

    sync_flag_locked = 1;

    cdvdman_stat.req.lba = lsn;
    cdvdman_stat.req.sectors = sectors;
    cdvdman_stat.req.sector_size = sector_size;
    cdvdman_stat.req.buf = buf;
    cdvdman_stat.req.source = source;

    CpuResumeIntr(OldState);

    if (IsIntrContext)
        iSignalSema(cdrom_rthread_sema);
    else
        SignalSema(cdrom_rthread_sema);

    return 1;
}

//-------------------------------------------------------------------------
static void cdvdman_initDiskType(void)
{
    cdvdman_stat.err = SCECdErNO;

    cdvdman_stat.disc_type_reg = (int)cdvdman_settings.media;
    M_DEBUG("DiskType=0x%x\n", cdvdman_settings.media);
}

//-------------------------------------------------------------------------
u32 sceCdPosToInt(sceCdlLOCCD *p)
{
    u32 result;

    result = ((u32)p->minute >> 4) * 10 + ((u32)p->minute & 0xF);
    result *= 60;
    result += ((u32)p->second >> 4) * 10 + ((u32)p->second & 0xF);
    result *= 75;
    result += ((u32)p->sector >> 4) * 10 + ((u32)p->sector & 0xF);
    result -= 150;

    M_DEBUG("%s({0x%X, 0x%X, 0x%X, 0x%X}) = %d\n", __FUNCTION__, p->minute, p->second, p->sector, p->track, result);

    return result;
}

//-------------------------------------------------------------------------
sceCdlLOCCD *sceCdIntToPos(u32 i, sceCdlLOCCD *p)
{
    u32 sc, se, mi;

    M_DEBUG("%s(%d)\n", __FUNCTION__, i);

    i += 150;
    se = i / 75;
    sc = i - se * 75;
    mi = se / 60;
    se = se - mi * 60;
    p->sector = (sc - (sc / 10) * 10) + (sc / 10) * 16;
    p->second = (se / 10) * 16 + se - (se / 10) * 10;
    p->minute = (mi / 10) * 16 + mi - (mi / 10) * 10;

    return p;
}

//-------------------------------------------------------------------------
sceCdCBFunc sceCdCallback(sceCdCBFunc func)
{
    int oldstate;
    void *old_cb;

    M_DEBUG("%s(0x%X)\n", __FUNCTION__, func);

    if (sceCdSync(1))
        return NULL;

    CpuSuspendIntr(&oldstate);

    old_cb = cb_data.user_cb;
    cb_data.user_cb = func;

    CpuResumeIntr(oldstate);

    return old_cb;
}

//-------------------------------------------------------------------------
int sceCdSC(int code, int *param)
{
    int result;

    M_DEBUG("%s(0x%X, 0x%X)\n", __FUNCTION__, code, *param);

    switch (code) {
        case CDSC_GET_INTRFLAG:
            result = cdvdman_stat.intr_ef;
            break;
        case CDSC_IO_SEMA:
            if (*param) {
                WaitSema(cdrom_io_sema);
            } else
                SignalSema(cdrom_io_sema);

            result = *param; // EE N-command code.
            break;
        case CDSC_GET_VERSION:
            result = CDVDMAN_MODULE_VERSION;
            break;
        case CDSC_GET_DEBUG_STATUS:
            *param = (int)&cdvdman_debug_print_flag;
            result = 0xFF;
            break;
        case CDSC_SET_ERROR:
            result = cdvdman_stat.err = *param;
            break;
        default:
            M_DEBUG("%s(0x%X, 0x%X) unknown code\n", __FUNCTION__, code, *param);
            result = 1; // dummy result
    }

    return result;
}

//-------------------------------------------------------------------------
static int cdvdman_writeSCmd(u8 cmd, const void *in, u16 in_size, void *out, u16 out_size)
{
    int i;
    u8 *p;

    WaitSema(cdvdman_scmdsema);

    if (CDVDreg_SDATAIN & 0x80) {
        SignalSema(cdvdman_scmdsema);
        return 0;
    }

    if (!(CDVDreg_SDATAIN & 0x40)) {
        do {
            (void)CDVDreg_SDATAOUT;
        } while (!(CDVDreg_SDATAIN & 0x40));
    }

    if (in_size > 0) {
        for (i = 0; i < in_size; i++) {
            p = (void *)((const u8 *)in + i);
            CDVDreg_SDATAIN = *p;
        }
    }

    CDVDreg_SCOMMAND = cmd;
    (void)CDVDreg_SCOMMAND;

    while (CDVDreg_SDATAIN & 0x80) {
        ;
    }

    i = 0;
    if (!(CDVDreg_SDATAIN & 0x40)) {
        do {
            if (i >= out_size) {
                break;
            }
            p = (void *)((u8 *)out + i);
            *p = CDVDreg_SDATAOUT;
            i++;
        } while (!(CDVDreg_SDATAIN & 0x40));
    }

    if (!(CDVDreg_SDATAIN & 0x40)) {
        do {
            (void)CDVDreg_SDATAOUT;
        } while (!(CDVDreg_SDATAIN & 0x40));
    }

    SignalSema(cdvdman_scmdsema);

    return 1;
}

//--------------------------------------------------------------
int cdvdman_sendSCmd(u8 cmd, const void *in, u16 in_size, void *out, u16 out_size)
{
    int r, retryCount = 0;

retry:

    r = cdvdman_writeSCmd(cmd & 0xff, in, in_size, out, out_size);
    if (r == 0) {
        DelayThread(2000);
        if (++retryCount <= 2500)
            goto retry;
    }

    DelayThread(2000);

    return 1;
}

//--------------------------------------------------------------
void cdvdman_cb_event(int reason)
{
    if (cb_data.user_cb != NULL) {
        cb_data.reason = reason;

        ClearEventFlag(cdvdman_stat.intr_ef, ~CDVDEF_CB_DONE);
        SetAlarm(&gCallbackSysClock, &event_alarm_cb, &cb_data);
        WaitEventFlag(cdvdman_stat.intr_ef, CDVDEF_CB_DONE, WEF_AND, NULL);
    }
}

//--------------------------------------------------------------
static unsigned int event_alarm_cb(void *args)
{
    struct cdvdman_cb_data *cb_data = args;

    if (cb_data->user_cb != NULL) // This interrupt does not occur immediately, hence check for the callback again here.
        cb_data->user_cb(cb_data->reason);

    iSetEventFlag(cdvdman_stat.intr_ef, CDVDEF_CB_DONE);

    return 0;
}

//--------------------------------------------------------------
static void cdvdman_cdread_Thread(void *args)
{
    cdvdman_read_t req;

    while (1) {
        WaitSema(cdrom_rthread_sema);
        memcpy(&req, &cdvdman_stat.req, sizeof(req));

        M_DEBUG("%s() [%d, %d, %d, %08x, %d]\n", __FUNCTION__, (int)req.lba, (int)req.sectors, (int)req.sector_size, (int)req.buf, (int)req.source);

        cdvdman_read(req.lba, req.sectors, req.sector_size, req.buf);

        sync_flag_locked = 0;
        SetEventFlag(cdvdman_stat.intr_ef, CDVDEF_MAN_UNLOCKED);

        switch (req.source) {
            case ECS_EXTERNAL:
                // Call from external irx (via sceCdRead)

                // Notify external irx that sceCdRead has finished
                cdvdman_cb_event(SCECdFuncRead);
                break;
            case ECS_SEARCHFILE:
            case ECS_EE_RPC:
                // Call from searchfile and ioops
                break;
            case ECS_STREAMING:
                // Call from streaming

                // The event will trigger the transmission of data to EE
                SetEventFlag(cdvdman_stat.intr_ef, CDVDEF_STM_DONE);

                // The callback will trigger a new read (if needed)
                if (Stm0Callback != NULL)
                    Stm0Callback();

                if (cdvdman_settings.flags & CDVDMAN_COMPAT_F1_2001)
                    cdvdman_cb_event(SCECdFuncRead);

                break;
        }

        M_DEBUG("%s() done\n", __FUNCTION__);
    }
}

//-------------------------------------------------------------------------
static void cdvdman_startThreads(void)
{
    iop_thread_t thread_param;

    cdvdman_stat.status = SCECdStatPause;
    cdvdman_stat.err = SCECdErNO;

    thread_param.thread = &cdvdman_cdread_Thread;
    thread_param.stacksize = 0x1000;
    thread_param.priority = 8;
    thread_param.attr = TH_C;
    thread_param.option = 0xABCD0000;

    cdvdman_ReadingThreadID = CreateThread(&thread_param);
    StartThread(cdvdman_ReadingThreadID, NULL);
}

//-------------------------------------------------------------------------
static void cdvdman_create_semaphores(void)
{
    iop_sema_t smp;

    smp.initial = 1;
    smp.max = 1;
    smp.attr = 0;
    smp.option = 0;

    cdvdman_scmdsema = CreateSema(&smp);
    smp.initial = 0;
    cdrom_rthread_sema = CreateSema(&smp);
}

//-------------------------------------------------------------------------
static int intrh_cdrom(void *common)
{
    if (CDVDreg_PWOFF & CDL_DATA_RDY)
        CDVDreg_PWOFF = CDL_DATA_RDY;

    if (CDVDreg_PWOFF & CDL_DATA_END) {
        iSetEventFlag(cdvdman_stat.intr_ef, CDVDEF_STM_DONE|CDVDEF_FSV_S596|CDVDEF_POWER_OFF); // Notify FILEIO and CDVDFSV of the power-off event.
    } else
        CDVDreg_PWOFF = CDL_DATA_COMPLETE; // Acknowledge interrupt

    return 1;
}

static inline void InstallIntrHandler(void)
{
    RegisterIntrHandler(IOP_IRQ_CDVD, 1, &intrh_cdrom, NULL);
    EnableIntr(IOP_IRQ_CDVD);

    // Acknowledge hardware events (e.g. poweroff)
    if (CDVDreg_PWOFF & CDL_DATA_END)
        CDVDreg_PWOFF = CDL_DATA_END;
    if (CDVDreg_PWOFF & CDL_DATA_RDY)
        CDVDreg_PWOFF = CDL_DATA_RDY;
}

int _start(int argc, char **argv)
{
    // register exports
    RegisterLibraryEntries(&_exp_cdvdman);
    RegisterLibraryEntries(&_exp_cdvdstm);

    // Setup the callback timer.
    USec2SysClock((cdvdman_settings.flags & CDVDMAN_COMPAT_FAST_READS) ? 0 : 5000, &gCallbackSysClock);

    // Limit min/max sectors
    if (cdvdman_settings.fs_sectors < 2)
        cdvdman_settings.fs_sectors = 2;
    if (cdvdman_settings.fs_sectors > 128)
        cdvdman_settings.fs_sectors = 128;
    cdvdman_fs_buf = AllocSysMemory(0, cdvdman_settings.fs_sectors * 2048 + CDVDMAN_FS_BUF_ALIGNMENT, NULL);

    // create SCMD/searchfile semaphores
    cdvdman_create_semaphores();

    // start cdvdman threads
    cdvdman_startThreads();

    // register cdrom device driver
    cdvdman_initdev();
    InstallIntrHandler();

    // init disk type stuff
    cdvdman_initDiskType();

    return MODULE_RESIDENT_END;
}

//-------------------------------------------------------------------------
void SetStm0Callback(StmCallback_t callback)
{
    Stm0Callback = callback;
}

//-------------------------------------------------------------------------
int _shutdown(void)
{
    return 0;
}
