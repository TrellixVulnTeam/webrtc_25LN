#!/usr/bin/env python

# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Aggregates Jacoco coverage files to produce output."""

from __future__ import print_function

import argparse
import fnmatch
import json
import os
import sys

import devil_chromium
from devil.utils import cmd_helper
from pylib.constants import host_paths

# Source paths should be passed to Jacoco in a way that the relative file paths
# reflect the class package name.
_PARTIAL_PACKAGE_NAMES = ['com/google', 'org/chromium']

# The sources_json_file is generated by jacoco_instr.py with source directories
# and input path to non-instrumented jars.
# e.g.
# 'source_dirs': [
# "chrome/android/java/src/org/chromium/chrome/browser/toolbar/bottom",
# "chrome/android/java/src/org/chromium/chrome/browser/ui/system",
# ...]
# 'input_path':
#   '$CHROMIUM_OUTPUT_DIR/\
#    obj/chrome/android/features/tab_ui/java__process_prebuilt-filtered.jar'
_SOURCES_JSON_FILES_SUFFIX = '__jacoco_sources.json'


def _GetFilesWithSuffix(root_dir, suffix):
  """Gets all files with a given suffix.

  Args:
    root_dir: Directory in which to search for files.
    suffix: Suffix to look for.

  Returns:
    A list of absolute paths to files that match.
  """
  files = []
  for root, _, filenames in os.walk(root_dir):
    basenames = fnmatch.filter(filenames, '*' + suffix)
    files.extend([os.path.join(root, basename) for basename in basenames])

  return files


def _AddArguments(parser):
  """Adds arguments related to parser.

  Args:
    parser: ArgumentParser object.
  """
  parser.add_argument(
      '--format',
      required=True,
      choices=['html', 'xml', 'csv'],
      help='Output report format. Choose one from html, xml and csv.')
  parser.add_argument('--output-dir', help='html report output directory.')
  parser.add_argument('--output-file', help='xml or csv report output file.')
  parser.add_argument(
      '--coverage-dir',
      required=True,
      help=('Root of the directory in which to search for '
            'coverage data (.exec) files.'))
  parser.add_argument(
      '--metadata-dir',
      help=('Root of the directory in which to search for '
            '*-filtered.jar and *__jacoco_sources.txt files.'))
  parser.add_argument(
      '--class-files',
      nargs='+',
      help='Location of Java non-instrumented class files. '
      'Use non-instrumented jars instead of instrumented jars. '
      'e.g. use chrome_java__process_prebuilt-filtered.jar instead of'
      'chrome_java__process_prebuilt-instrumented.jar')
  parser.add_argument(
      '--sources',
      nargs='+',
      help='Location of the source files. '
      'Specified source folders must be the direct parent of the folders '
      'that define the Java packages.'
      'e.g. <src_dir>/chrome/android/java/src/')
  parser.add_argument(
      '--cleanup',
      action='store_true',
      help=('If set, removes coverage files generated at '
            'runtime.'))


def main():
  parser = argparse.ArgumentParser()
  _AddArguments(parser)
  args = parser.parse_args()

  if args.format == 'html':
    if not args.output_dir:
      parser.error('--output-dir needed for html report.')
  elif not args.output_file:
    parser.error('--output-file needed for xml or csv report.')

  if not (args.metadata_dir or args.class_files):
    parser.error('At least either --metadata-dir or --class-files needed.')

  devil_chromium.Initialize()

  coverage_files = _GetFilesWithSuffix(args.coverage_dir, '.exec')
  if not coverage_files:
    parser.error('Found no coverage file under %s' % args.coverage_dir)
  print('Found coverage files: %s' % str(coverage_files))

  class_files = []
  sources = []
  if args.metadata_dir:
    sources_json_files = _GetFilesWithSuffix(args.metadata_dir,
                                             _SOURCES_JSON_FILES_SUFFIX)
    for f in sources_json_files:
      with open(f, 'r') as json_file:
        data = json.load(json_file)
        class_files.append(data['input_path'])
        sources.extend(data['source_dirs'])

  fixed_source_paths = set()

  for path in sources:
    for partial in _PARTIAL_PACKAGE_NAMES:
      if partial in path:
        fixed_path = os.path.join(host_paths.DIR_SOURCE_ROOT,
                                  path[:path.index(partial)])
        fixed_source_paths.add(fixed_path)
        break

  sources = list(fixed_source_paths)

  if args.class_files:
    class_files += args.class_files
  if args.sources:
    sources += args.sources

  cmd = [
      'java', '-jar',
      os.path.join(host_paths.DIR_SOURCE_ROOT, 'third_party', 'jacoco', 'lib',
                   'jacococli.jar'), 'report'
  ] + coverage_files
  for f in class_files:
    cmd += ['--classfiles', f]
  for source in sources:
    cmd += ['--sourcefiles', source]
  if args.format == 'html':
    cmd += ['--html', args.output_dir]
  elif args.format == 'xml':
    cmd += ['--xml', args.output_file]
  else:
    cmd += ['--csv', args.output_file]

  exit_code = cmd_helper.RunCmd(cmd)

  if args.cleanup:
    for f in coverage_files:
      os.remove(f)

  # Command tends to exit with status 0 when it actually failed.
  if not exit_code:
    if args.format == 'html':
      if not os.path.exists(args.output_dir) or not os.listdir(args.output_dir):
        print('No report generated at %s' % args.output_dir)
        exit_code = 1
    elif not os.path.exists(args.output_file):
      print('No report generated at %s' % args.output_file)
      exit_code = 1

  return exit_code


if __name__ == '__main__':
  sys.exit(main())
