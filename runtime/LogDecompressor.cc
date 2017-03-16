/* Copyright (c) 2016-2017 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <cstdlib>
#include <fstream>
#include <vector>

#include <string.h>
#include <stdarg.h>

#include "Log.h"
#include "Cycles.h"

// File generated by the NanoLog preprocessor that contains all the
// compression and decompression functions.
#include "GeneratedCode.h"

using namespace Log;

/**
 * Find all the original NANO_LOG format strings in the user sources that
 * statically contain the searchString and print them out in the format
 * "id   | filename | line | format string"
 *
 * \param searchString
 *      Static string to search for in the format strings
 */
void
printLogMetadataContainingSubstring(std::string searchString)
{
    std::vector<size_t> matchingLogIds;

    for (size_t i = 0; i < GeneratedFunctions::numLogIds; ++i) {
        const char *fmtMsg = GeneratedFunctions::logId2Metadata[i].fmtString;
        if (strstr(fmtMsg, searchString.c_str()))
            matchingLogIds.push_back(i);
    }

    printf("%4s | %-20s | %-4s | %s\r\n", "id", "filename", "line",
                                                            "format string");
    for (auto id : matchingLogIds) {
        GeneratedFunctions::LogMetadata lm = GeneratedFunctions::logId2Metadata[id];
        printf("%4lu | %-20s | %-4u | %s\r\n", id, lm.fileName, lm.lineNumber,
                                                                lm.fmtString);
    }
}

/**
 * Simple program to decompress log files produced by the NanoLog System.
 * Note that this executable must be compiled with the same BufferStuffer.h
 * as the LogCompressor that generated the compressedLog for this to work.
 */
int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Decompresses log files produced by the NanoLog System\r\n"
                "into a human readable format.\r\n\r\n");
        printf("\tUsage: %s <logFile> [# messages to print]\r\n", argv[0]);
        exit(1);
    }

    int msgsToPrint = 0;
    if (argc > 2) {
        try {
            msgsToPrint = std::stoi(argv[2]);
        } catch (const std::invalid_argument& e) {
            printf("Invalid # of message to print, please enter a number:"
                    " %s\r\n",  argv[2]);
            exit(-1);
        } catch (const std::out_of_range& e) {
            printf("# of messages to print is too large: %s\r\n", argv[2]);
            printf("If you intend to print all message, "
                    "exclude the # messages to print parameter.\r\n");
            exit(-1);
        }

        if (msgsToPrint < 0) {
            printf("# of messages to print must be positive: %s\r\n", argv[2]);
            exit(-1);
        }
    }

    if (msgsToPrint == 0)
        msgsToPrint = -1;

    Log::Decoder decoder;
    if(!decoder.open(argv[1])) {
        printf("Unable to open file %s\r\n", argv[1]);
    } else {
        decoder.decompressUnordered(stdout, msgsToPrint);
    }

    return 0;
}

