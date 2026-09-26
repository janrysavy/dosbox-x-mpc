// Included in cmos.cpp so tests exercise the production serializer and timer.
#include <gtest/gtest.h>
#include <sstream>
#include <vector>

namespace {
class CmosState : public ::testing::Test {
protected:
    SaveState::MemoryImage original_machine;
    bool captured = false;
    decltype(cmos) original_cmos;
    bool original_sync = false;
    bool original_nmi_gate = false;
    bool original_sync_flag = false;
    uint8_t original_sync_sec = 0, original_sync_min = 0, original_sync_hour = 0;

    void SetUp() override {
        std::string error;
        original_cmos = cmos;
        original_sync = sync_time;
        original_nmi_gate = CPU_NMI_gate;
        original_sync_flag = cmos_sync_flag;
        original_sync_sec = cmos_sync_sec;
        original_sync_min = cmos_sync_min;
        original_sync_hour = cmos_sync_hour;
        captured = SaveState::instance().captureMemory(original_machine, error);
        ASSERT_TRUE(captured) << error;
        sync_time = false; // Host wall time is deliberately outside this replay test.
    }

    void TearDown() override {
        std::string error;
        if (captured)
            EXPECT_TRUE(SaveState::instance().restoreMemory(original_machine, error)) << error;
        // Also isolate the old-serializer negative-control run, which omits these.
        cmos = original_cmos;
        sync_time = original_sync;
        CPU_NMI_gate = original_nmi_gate;
        cmos_sync_flag = original_sync_flag;
        cmos_sync_sec = original_sync_sec;
        cmos_sync_min = original_sync_min;
        cmos_sync_hour = original_sync_hour;
    }

    static void Write(uint8_t reg, uint8_t value) {
        IO_WriteB(0x70, reg);
        IO_WriteB(0x71, value);
    }
    static uint8_t Read(uint8_t reg) {
        IO_WriteB(0x70, reg);
        return static_cast<uint8_t>(IO_ReadB(0x71));
    }
    static std::string Save() {
        std::ostringstream stream(std::ios::binary | std::ios::out);
        static_cast<SaveState::Component&>(dummy).getBytes(stream);
        EXPECT_TRUE(stream.good());
        return stream.str();
    }
    static void Load(const std::string& bytes) {
        std::istringstream stream(bytes, std::ios::binary | std::ios::in);
        static_cast<SaveState::Component&>(dummy).setBytes(stream);
        EXPECT_TRUE(stream.good());
        EXPECT_EQ(std::char_traits<char>::eof(), stream.peek());
    }
    static void SetClock() {
        Write(0x0b, 0x86); // Locked, binary, 24-hour.
        Write(0x32, 20);
        Write(0x09, 0);
        Write(0x08, 2);
        Write(0x07, 27);
        Write(0x06, 2);
        Write(0x04, 23);
        Write(0x02, 59);
        Write(0x00, 59);
        Write(0x05, 0);
        Write(0x03, 0);
        Write(0x01, 0);
        cmos.clock_time_t = 951695999;
        cmos_sync_flag = true;
        cmos_sync_sec = 51;
        cmos_sync_min = 42;
        cmos_sync_hour = 13;
        Read(0x0c); // Clear any preceding interrupt flags.
    }
    static void Mutate() {
        Write(0x0b, 0x00); // Unlocked, BCD, 12-hour.
        Write(0x00, 0x12);
        Write(0x02, 0x34);
        Write(0x04, 0x05);
        Write(0x07, 0x17);
        Write(0x08, 0x08);
        Write(0x09, 0x23);
        Write(0x32, 0x19);
        Write(0x01, 0x13);
        Write(0x03, 0x14);
        Write(0x05, 0x03);
        cmos.clock_time_t = 1;
        cmos_sync_flag = false;
        cmos_sync_sec = cmos_sync_min = cmos_sync_hour = 0;
    }
    static void TimerBoundary() {
        // Invoke the real callback at a due one-second boundary without running
        // the guest or changing host time. Restore the full PIC queue in teardown.
        PIC_RemoveEvents(cmos_timerevent);
        cmos.last.ended = static_cast<double>(PIC_FullIndex()) - 1000.0;
        cmos_timerevent(0);
    }
    static std::vector<uint8_t> ClockPorts() {
        std::vector<uint8_t> result;
        for (const auto reg : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x32, 0x0b})
            result.push_back(Read(static_cast<uint8_t>(reg)));
        return result;
    }
};

TEST_F(CmosState, RestoresClockModeAlarmAndPendingSync) {
    SetClock();
    const auto expected = ClockPorts();
    IO_WriteB(0x70, 0x02); // Capture a partially completed index/data transaction.
    const auto bytes = Save();
    Mutate();
    Load(bytes);
    EXPECT_EQ(59, IO_ReadB(0x71));
    EXPECT_EQ(expected, ClockPorts());
    EXPECT_EQ(951695999, cmos.clock_time_t);
    EXPECT_TRUE(cmos_sync_flag);
    EXPECT_EQ(51, cmos_sync_sec);
    EXPECT_EQ(42, cmos_sync_min);
    EXPECT_EQ(13, cmos_sync_hour);
    EXPECT_TRUE(cmos.lock);
    EXPECT_FALSE(cmos.bcd);
    EXPECT_FALSE(cmos.ampm);
}

TEST_F(CmosState, LockedTimerDoesNotAdvanceAfterRestore) {
    SetClock();
    const auto expected = ClockPorts();
    const auto bytes = Save();
    TimerBoundary();
    EXPECT_EQ(expected, ClockPorts());
    Mutate();
    Load(bytes);
    TimerBoundary();
    EXPECT_EQ(expected, ClockPorts());
    EXPECT_EQ(951695999, cmos.clock_time_t);
}

TEST_F(CmosState, RestoredMidnightMatchesUninterruptedClockAndAlarm) {
    SetClock();
    Write(0x0b, 0x36); // Binary/24-hour, alarm and update interrupts, unlocked.
    const auto bytes = Save();
    TimerBoundary();
    const auto expected = ClockPorts();
    EXPECT_EQ(28, Read(0x07));
    EXPECT_EQ(2, Read(0x08));
    EXPECT_EQ(0, Read(0x00));
    EXPECT_EQ(0, Read(0x02));
    EXPECT_EQ(0, Read(0x04));
    const auto status = Read(0x0c);
    EXPECT_EQ(0xb0, status); // IRQF, alarm and update-ended flags.
    EXPECT_EQ(0, Read(0x0c));
    const auto time = cmos.clock_time_t;
    Mutate();
    Load(bytes);
    TimerBoundary();
    EXPECT_EQ(expected, ClockPorts());
    EXPECT_EQ(status, Read(0x0c));
    EXPECT_EQ(0, Read(0x0c));
    EXPECT_EQ(time, cmos.clock_time_t);
}

TEST_F(CmosState, RestoresBcdAndTwelveHourWriteInterpretation) {
    SetClock();
    Write(0x0b, 0x80); // Locked, BCD, 12-hour.
    const auto bytes = Save();
    Write(0x04, 0x09);
    Write(0x00, 0x47);
    const auto expected = ClockPorts();
    Write(0x0b, 0x06); // Change both decoded mode flags.
    Load(bytes);
    Write(0x04, 0x09);
    Write(0x00, 0x47);
    EXPECT_EQ(expected, ClockPorts());
    EXPECT_EQ(47, cmos.clock.sec);
    EXPECT_TRUE(cmos.bcd);
    EXPECT_TRUE(cmos.ampm);
}
TEST_F(CmosState, CalendarKeepsLastDayAndDecemberBeforeRollingOver) {
    struct CalendarCase {
        uint16_t year;
        uint8_t month, day;
        uint16_t next_year;
        uint8_t next_month, next_day;
    };
    const CalendarCase cases[] = {
        {2000, 2, 28, 2000, 2, 29}, // Gregorian century leap year.
        {2000, 2, 29, 2000, 3, 1},
        {1900, 2, 27, 1900, 2, 28}, // Century exception: not a leap year.
        {1900, 2, 28, 1900, 3, 1},
        {2024, 2, 28, 2024, 2, 29}, // Ordinary leap year.
        {2023, 4, 29, 2023, 4, 30},
        {2023, 4, 30, 2023, 5, 1},
        {2023, 1, 30, 2023, 1, 31},
        {2023, 1, 31, 2023, 2, 1},
        {2023, 11, 30, 2023, 12, 1},
        {2023, 12, 30, 2023, 12, 31},
        {2023, 12, 31, 2024, 1, 1},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(::testing::Message() << test.year << "-"
                     << unsigned(test.month) << "-" << unsigned(test.day));
        SetClock();
        Write(0x32, static_cast<uint8_t>(test.year / 100));
        Write(0x09, static_cast<uint8_t>(test.year % 100));
        Write(0x08, test.month);
        Write(0x07, test.day);
        Write(0x06, 7); // Verify weekday rollover independently of month.
        Write(0x0b, 0x06); // Binary/24-hour, unlocked.
        TimerBoundary();
        EXPECT_EQ(test.next_year / 100, Read(0x32));
        EXPECT_EQ(test.next_year % 100, Read(0x09));
        EXPECT_EQ(test.next_month, Read(0x08));
        EXPECT_EQ(test.next_day, Read(0x07));
        EXPECT_EQ(1, Read(0x06));
        EXPECT_EQ(0, Read(0x04));
        EXPECT_EQ(0, Read(0x02));
        EXPECT_EQ(0, Read(0x00));
    }
}
} // namespace
