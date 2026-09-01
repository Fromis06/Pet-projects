#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CalendarEvent {
    std::wstring uid;
    std::wstring title;
    std::wstring description;
    std::wstring location;
    std::int64_t start = 0;
    std::int64_t end = 0;
    bool allDay = false;
    std::string recurrenceRule;
    std::vector<std::int64_t> exceptionDates;
};

struct CalendarParseResult {
    std::vector<CalendarEvent> events;
    std::vector<std::wstring> warnings;
    bool opened = false;
};

CalendarParseResult LoadIcsFile(const std::wstring& path);
std::vector<CalendarEvent> EventsInRange(
    const std::vector<CalendarEvent>& source,
    std::int64_t rangeStart,
    std::int64_t rangeEnd);
