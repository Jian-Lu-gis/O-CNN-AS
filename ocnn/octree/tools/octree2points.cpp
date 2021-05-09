﻿#include <iostream>
#include <string>
#include <vector>

#include "util.h"
#include "points.h"
#include "octree.h"
#include "cmd_flags.h"

using namespace std;

DEFINE_string(filenames, kRequired, "", "The input filenames");
DEFINE_string(output_path, kOptional, ".", "The output path");
DEFINE_int(depth_start, kOptional, 0, "The starting depth");
DEFINE_int(depth_end, kOptional, 10, "The ending depth");
DEFINE_bool(verbose, kOptional, true, "Output logs");


int main(int argc, char* argv[]) {
  bool succ = cflags::ParseCmd(argc, argv);
  if (!succ) {
    cflags::PrintHelpInfo("\nUsage: octree2points.exe");
    return 0;
  }

  // file path
  string file_path = FLAGS_filenames;
  string output_path = FLAGS_output_path;
  if (output_path != ".") mkdir(output_path);
  else output_path = extract_path(file_path);
  output_path += "/";

  vector<string> all_files;
  get_all_filenames(all_files, file_path);

  for (int i = 0; i < all_files.size(); i++) {
    string filename = extract_filename(all_files[i]);;
    if (FLAGS_verbose) cout << "Processing: " << filename << std::endl;

    // load octree
    Octree octree;
    bool succ = octree.read_octree(all_files[i]);
    if (!succ) {
      if (FLAGS_verbose) cout << "Can not load " << filename << std::endl;
      continue;
    }
    string msg;
    succ = octree.info().check_format(msg);
    if (!succ) {
      if (FLAGS_verbose) cout << filename << std::endl << msg << std::endl;
      continue;
    }

    // convert
    Points pts;
    octree.octree2pts(pts, FLAGS_depth_start, FLAGS_depth_end);

    // save points
    filename = output_path + filename + ".points";
    pts.write_points(filename);
  }

  return 0;
}
