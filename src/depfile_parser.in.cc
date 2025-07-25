// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "depfile_parser.h"
#include "util.h"

#include <algorithm>

using namespace std;

DepfileParser::DepfileParser(DepfileParserOptions options)
  : options_(options)
{
}

// A note on backslashes in Makefiles, from reading the docs:
// Backslash-newline is the line continuation character.
// Backslash-# escapes a # (otherwise meaningful as a comment start).
// Backslash-% escapes a % (otherwise meaningful as a special).
// Finally, quoting the GNU manual, "Backslashes that are not in danger
// of quoting ‘%’ characters go unmolested."
// How do you end a line with a backslash?  The netbsd Make docs suggest
// reading the result of a shell command echoing a backslash!
//
// Rather than implement all of above, we follow what GCC/Clang produces:
// Backslashes escape a space or hash sign.
// When a space is preceded by 2N+1 backslashes, it is represents N backslashes
// followed by space.
// When a space is preceded by 2N backslashes, it represents 2N backslashes at
// the end of a filename.
// A hash sign is escaped by a single backslash. All other backslashes remain
// unchanged.
//
// If anyone actually has depfiles that rely on the more complicated
// behavior we can adjust this.
bool DepfileParser::Parse(string* content, string* err) {
  // in: current parser input point.
  // end: end of input.
  // parsing_targets: whether we are parsing targets or dependencies.
  char* in = &(*content)[0];
  char* end = in + content->size();
  bool have_target = false;
  bool parsing_targets = true;
  bool poisoned_input = false;
  bool is_empty = true;
  while (in < end) {
    bool have_newline = false;
    // out: current output point (typically same as in, but can fall behind
    // as we de-escape backslashes).
    char* out = in;
    // filename: start of the current parsed filename.
    char* filename = out;
    for (;;) {
      // start: beginning of the current parsed span.
      const char* start = in;
      char* yymarker = NULL;
      /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:define:YYCURSOR = in;
      re2c:define:YYLIMIT = end;
      re2c:define:YYMARKER = yymarker;

      re2c:yyfill:enable = 0;

      re2c:indent:top = 2;
      re2c:indent:string = "  ";

      nul = "\000";
      newline = '\r'?'\n';

      '\\\\'* '\\ ' {
        // 2N+1 backslashes plus space -> N backslashes plus space.
        int len = (int)(in - start);
        int n = len / 2 - 1;
        if (out < start)
          memset(out, '\\', n);
        out += n;
        *out++ = ' ';
        continue;
      }
      '\\\\'+ ' ' {
        // 2N backslashes plus space -> 2N backslashes, end of filename.
        int len = (int)(in - start);
        if (out < start)
          memset(out, '\\', len - 1);
        out += len - 1;
        break;
      }
      '\\'+ '#' {
        // De-escape hash sign, but preserve other leading backslashes.
        int len = (int)(in - start);
        if (len > 2 && out < start)
          memset(out, '\\', len - 2);
        out += len - 2;
        *out++ = '#';
        continue;
      }
      '\\'+ ':' [\x00\x20\r\n\t] {
        // Backslash followed by : and whitespace.
        // It is therefore normal text and not an escaped colon
        int len = (int)(in - start - 1);
        // Need to shift it over if we're overwriting backslashes.
        if (out < start)
          memmove(out, start, len);
        out += len;
        if (*(in - 1) == '\n')
          have_newline = true;
        break;
      }
      '\\'+ ':' {
        // De-escape colon sign, but preserve other leading backslashes.
        // Regular expression uses lookahead to make sure that no whitespace
        // nor EOF follows. In that case it'd be the : at the end of a target
        int len = (int)(in - start);
        if (len > 2 && out < start)
          memset(out, '\\', len - 2);
        out += len - 2;
        *out++ = ':';
        continue;
      }
      '$$' {
        // De-escape dollar character.
        *out++ = '$';
        continue;
      }
      '\\'+ [^\000\r\n] | [a-zA-Z0-9+?"'&,/_:.~()}{%=@\x5B\x5D!\x80-\xFF-]+ {
        // Got a span of plain text.
        int len = (int)(in - start);
        // Need to shift it over if we're overwriting backslashes.
        if (out < start)
          memmove(out, start, len);
        out += len;
        continue;
      }
      nul {
        break;
      }
      '\\' newline {
        // A line continuation ends the current file name.
        break;
      }
      newline {
        // A newline ends the current file name and the current rule.
        have_newline = true;
        break;
      }
      [^] {
        // For any other character (e.g. whitespace), swallow it here,
        // allowing the outer logic to loop around again.
        break;
      }
      */
    }

    int len = (int)(out - filename);
    const bool is_dependency = !parsing_targets;
    if (len > 0 && filename[len - 1] == ':') {
      len--;  // Strip off trailing colon, if any.
      parsing_targets = false;
      have_target = true;
    }

    if (len > 0) {
      is_empty = false;
      StringPiece piece = StringPiece(filename, len);
      // If we've seen this as an input before, skip it.
      std::vector<StringPiece>::iterator pos = std::find(ins_.begin(), ins_.end(), piece);
      if (pos == ins_.end()) {
        if (is_dependency) {
          if (poisoned_input) {
            *err = "inputs may not also have inputs";
            return false;
          }
          // New input.
          ins_.push_back(piece);
        } else {
          // Check for a new output.
          if (std::find(outs_.begin(), outs_.end(), piece) == outs_.end())
            outs_.push_back(piece);
        }
      } else if (!is_dependency) {
        // We've passed an input on the left side; reject new inputs.
        poisoned_input = true;
      }
    }

    if (have_newline) {
      // A newline ends a rule so the next filename will be a new target.
      parsing_targets = true;
      poisoned_input = false;
    }
  }
  if (!have_target && !is_empty) {
    *err = "expected ':' in depfile";
    return false;
  }
  return true;
}
