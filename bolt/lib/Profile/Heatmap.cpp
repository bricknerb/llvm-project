//===- bolt/Profile/Heatmap.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Profile/Heatmap.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

#define DEBUG_TYPE "bolt-heatmap"

using namespace llvm;

namespace llvm {
namespace bolt {

void Heatmap::registerAddressRange(uint64_t StartAddress, uint64_t EndAddress,
                                   uint64_t Count) {
  if (ignoreAddress(StartAddress)) {
    ++NumSkippedRanges;
    return;
  }

  if (StartAddress > EndAddress || EndAddress - StartAddress > 64 * 1024) {
    LLVM_DEBUG(dbgs() << "invalid range : 0x" << Twine::utohexstr(StartAddress)
                      << " -> 0x" << Twine::utohexstr(EndAddress) << '\n');
    ++NumSkippedRanges;
    return;
  }

  for (uint64_t Bucket = StartAddress / BucketSize;
       Bucket <= EndAddress / BucketSize; ++Bucket)
    Map[Bucket] += Count;
}

void Heatmap::print(StringRef FileName) const {
  std::error_code EC;
  raw_fd_ostream OS(FileName, EC, sys::fs::OpenFlags::OF_None);
  if (EC) {
    errs() << "error opening output file: " << EC.message() << '\n';
    exit(1);
  }
  print(OS);
}

void Heatmap::print(raw_ostream &OS) const {
  const char FillChar = '.';

  const auto DefaultColor = raw_ostream::WHITE;
  auto changeColor = [&](raw_ostream::Colors Color) -> void {
    static auto CurrentColor = raw_ostream::BLACK;
    if (CurrentColor == Color)
      return;
    OS.changeColor(Color);
    CurrentColor = Color;
  };

  const uint64_t BytesPerLine = opts::BucketsPerLine * BucketSize;

  // Calculate the max value for scaling.
  uint64_t MaxValue = 0;
  for (const std::pair<const uint64_t, uint64_t> &Entry : Map)
    MaxValue = std::max<uint64_t>(MaxValue, Entry.second);

  // Print start of the line and fill it with an empty space right before
  // the Address.
  auto startLine = [&](uint64_t Address, bool Empty = false) {
    changeColor(DefaultColor);
    const uint64_t LineAddress = Address / BytesPerLine * BytesPerLine;

    if (MaxAddress > 0xffffffff)
      OS << format("0x%016" PRIx64 ": ", LineAddress);
    else
      OS << format("0x%08" PRIx64 ": ", LineAddress);

    if (Empty)
      Address = LineAddress + BytesPerLine;
    for (uint64_t Fill = LineAddress; Fill < Address; Fill += BucketSize)
      OS << FillChar;
  };

  // Finish line after \p Address was printed.
  auto finishLine = [&](uint64_t Address) {
    const uint64_t End = alignTo(Address + 1, BytesPerLine);
    for (uint64_t Fill = Address + BucketSize; Fill < End; Fill += BucketSize)
      OS << FillChar;
    OS << '\n';
  };

  // Fill empty space in (Start, End) range.
  auto fillRange = [&](uint64_t Start, uint64_t End) {
    if ((Start / BytesPerLine) == (End / BytesPerLine)) {
      for (uint64_t Fill = Start + BucketSize; Fill < End; Fill += BucketSize) {
        changeColor(DefaultColor);
        OS << FillChar;
      }
      return;
    }

    changeColor(DefaultColor);
    finishLine(Start);
    Start = alignTo(Start, BytesPerLine);

    uint64_t NumEmptyLines = (End - Start) / BytesPerLine;

    if (NumEmptyLines > 32) {
      OS << '\n';
    } else {
      while (NumEmptyLines--) {
        startLine(Start, /*Empty=*/true);
        OS << '\n';
        Start += BytesPerLine;
      }
    }

    startLine(End);
  };

  static raw_ostream::Colors Colors[] = {
      raw_ostream::WHITE, raw_ostream::WHITE,  raw_ostream::CYAN,
      raw_ostream::GREEN, raw_ostream::YELLOW, raw_ostream::RED};
  constexpr size_t NumRanges = sizeof(Colors) / sizeof(Colors[0]);

  uint64_t Range[NumRanges];
  for (uint64_t I = 0; I < NumRanges; ++I)
    Range[I] = std::max(I + 1, (uint64_t)std::pow((double)MaxValue,
                                                  (double)(I + 1) / NumRanges));
  Range[NumRanges - 1] = std::max((uint64_t)NumRanges, MaxValue);

  // Print scaled value
  auto printValue = [&](uint64_t Value, char Character, bool ResetColor) {
    assert(Value && "should only print positive values");
    for (unsigned I = 0; I < sizeof(Range) / sizeof(Range[0]); ++I) {
      if (Value <= Range[I]) {
        changeColor(Colors[I]);
        break;
      }
    }
    if (Value <= Range[0])
      OS << static_cast<char>(std::tolower(Character));
    else
      OS << static_cast<char>(std::toupper(Character));

    if (ResetColor)
      changeColor(DefaultColor);
  };

  // Print against black background
  OS.changeColor(raw_ostream::BLACK, /*Bold=*/false, /*Background=*/true);
  changeColor(DefaultColor);

  // Print map legend
  OS << "Legend:\n";
  OS << "\nRanges:\n";
  uint64_t PrevValue = 0;
  for (unsigned I = 0; I < sizeof(Range) / sizeof(Range[0]); ++I) {
    const uint64_t Value = Range[I];
    OS << "  ";
    printValue(Value, 'o', /*ResetColor=*/true);
    OS << " : (" << PrevValue << ", " << Value << "]\n";
    PrevValue = Value;
  }
  if (opts::HeatmapPrintMappings) {
    OS << "\nSections:\n";
    unsigned SectionIdx = 0;
    for (auto TxtSeg : TextSections) {
      const char Upper = static_cast<char>('A' + ((SectionIdx++) % 26));
      const char Lower = static_cast<char>(std::tolower(Upper));
      OS << formatv("  {0}/{1} : {2,-10} ", Lower, Upper, TxtSeg.Name);
      if (MaxAddress > 0xffffffff)
        OS << format("0x%016" PRIx64, TxtSeg.BeginAddress) << "-"
           << format("0x%016" PRIx64, TxtSeg.EndAddress) << "\n";
      else
        OS << format("0x%08" PRIx64, TxtSeg.BeginAddress) << "-"
           << format("0x%08" PRIx64, TxtSeg.EndAddress) << "\n";
    }
    OS << "\n";
  }

  // Pos - character position from right in hex form.
  auto printHeader = [&](unsigned Pos) {
    OS << "            ";
    if (MaxAddress > 0xffffffff)
      OS << "        ";
    unsigned PrevValue = unsigned(-1);
    for (unsigned I = 0; I < BytesPerLine; I += BucketSize) {
      const unsigned Value = (I & ((1 << Pos * 4) - 1)) >> (Pos - 1) * 4;
      if (Value != PrevValue) {
        OS << Twine::utohexstr(Value);
        PrevValue = Value;
      } else {
        OS << ' ';
      }
    }
    OS << '\n';
  };
  for (unsigned I = 5; I > 0; --I)
    printHeader(I);

  auto SectionStart = TextSections.begin();
  uint64_t PrevAddress = 0;
  for (auto MI = Map.begin(), ME = Map.end(); MI != ME; ++MI) {
    const std::pair<const uint64_t, uint64_t> &Entry = *MI;
    uint64_t Address = Entry.first * BucketSize;
    char Character = 'o';

    // Check if address is in the current or any later section.
    auto Section = std::find_if(
        SectionStart, TextSections.end(), [&](const SectionNameAndRange &S) {
          return Address >= S.BeginAddress && Address < S.EndAddress;
        });
    if (Section != TextSections.end()) {
      // Shift the section forward (if SectionStart is different from Section).
      // This works, because TextSections is sorted by start address.
      SectionStart = Section;
      Character = 'a' + ((Section - TextSections.begin()) % 26);
    }

    if (PrevAddress)
      fillRange(PrevAddress, Address);
    else
      startLine(Address);

    printValue(Entry.second, Character, /*ResetColor=*/false);

    PrevAddress = Address;
  }

  if (PrevAddress) {
    changeColor(DefaultColor);
    finishLine(PrevAddress);
  }
}

void Heatmap::printCDF(StringRef FileName) const {
  std::error_code EC;
  raw_fd_ostream OS(FileName, EC, sys::fs::OpenFlags::OF_None);
  if (EC) {
    errs() << "error opening output file: " << EC.message() << '\n';
    exit(1);
  }
  printCDF(OS);
}

void Heatmap::printCDF(raw_ostream &OS) const {
  uint64_t NumTotalCounts = 0;
  std::vector<uint64_t> Counts;

  for (const std::pair<const uint64_t, uint64_t> &KV : Map) {
    Counts.push_back(KV.second);
    NumTotalCounts += KV.second;
  }

  llvm::sort(Counts, std::greater<uint64_t>());

  double RatioLeftInKB = (1.0 * BucketSize) / 1024;
  assert(NumTotalCounts > 0 &&
         "total number of heatmap buckets should be greater than 0");
  double RatioRightInPercent = 100.0 / NumTotalCounts;
  uint64_t RunningCount = 0;

  OS << "Bucket counts, Size (KB), CDF (%)\n";
  for (uint64_t I = 0; I < Counts.size(); I++) {
    RunningCount += Counts[I];
    OS << format("%llu", (I + 1)) << ", "
       << format("%.4f", RatioLeftInKB * (I + 1)) << ", "
       << format("%.4f", RatioRightInPercent * (RunningCount)) << "\n";
  }

  Counts.clear();
}

void Heatmap::printSectionHotness(StringRef FileName) const {
  std::error_code EC;
  raw_fd_ostream OS(FileName, EC, sys::fs::OpenFlags::OF_None);
  if (EC) {
    errs() << "error opening output file: " << EC.message() << '\n';
    exit(1);
  }
  printSectionHotness(OS);
}

void Heatmap::printSectionHotness(raw_ostream &OS) const {
  uint64_t NumTotalCounts = 0;
  StringMap<uint64_t> SectionHotness;
  StringMap<uint64_t> BucketUtilization;
  unsigned TextSectionIndex = 0;

  if (TextSections.empty())
    return;

  uint64_t UnmappedHotness = 0;
  auto RecordUnmappedBucket = [&](uint64_t Address, uint64_t Frequency) {
    if (opts::Verbosity >= 1)
      errs() << "Couldn't map the address bucket [0x"
             << Twine::utohexstr(Address) << ", 0x"
             << Twine::utohexstr(Address + BucketSize) << "] containing "
             << Frequency << " samples to a text section in the binary.";
    UnmappedHotness += Frequency;
  };

  for (const std::pair<const uint64_t, uint64_t> &KV : Map) {
    NumTotalCounts += KV.second;
    // We map an address bucket to the first section (lowest address)
    // overlapping with that bucket.
    auto Address = KV.first * BucketSize;
    while (TextSectionIndex < TextSections.size() &&
           Address >= TextSections[TextSectionIndex].EndAddress)
      TextSectionIndex++;
    if (TextSectionIndex >= TextSections.size() ||
        Address + BucketSize < TextSections[TextSectionIndex].BeginAddress) {
      RecordUnmappedBucket(Address, KV.second);
      continue;
    }
    SectionHotness[TextSections[TextSectionIndex].Name] += KV.second;
    ++BucketUtilization[TextSections[TextSectionIndex].Name];
  }

  assert(NumTotalCounts > 0 &&
         "total number of heatmap buckets should be greater than 0");

  OS << "Section Name, Begin Address, End Address, Percentage Hotness, "
     << "Utilization Pct, Partition Score\n";
  const uint64_t MappedCounts = NumTotalCounts - UnmappedHotness;
  for (const auto [Name, Begin, End] : TextSections) {
    const float Hotness = 1. * SectionHotness[Name] / NumTotalCounts;
    const float MappedHotness =
        MappedCounts ? 1. * SectionHotness[Name] / MappedCounts : 0;
    const uint64_t NumBuckets =
        End / BucketSize + !!(End % BucketSize) - Begin / BucketSize;
    const float Utilization = 1. * BucketUtilization[Name] / NumBuckets;
    const float PartitionScore = MappedHotness * Utilization;
    OS << formatv("{0}, {1:x}, {2:x}, {3:f4}, {4:f4}, {5:f4}\n", Name, Begin,
                  End, 100. * Hotness, 100. * Utilization, PartitionScore);
  }
  if (UnmappedHotness > 0)
    OS << formatv("[unmapped], 0x0, 0x0, {0:f4}, 0, 0\n",
                  100.0 * UnmappedHotness / NumTotalCounts);
}
} // namespace bolt
} // namespace llvm
