#include "octree.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "marching_cube.h"


void Octree::build(const OctreeInfo& octree_info, const Points& point_cloud) {
  // init
  clear(octree_info.depth());
  oct_info_ = octree_info;

  // preprocess, get key and sort
  vector<float> pts_scaled;
  normalize_pts(pts_scaled, point_cloud);
  vector<uint32> node_keys, sorted_idx;
  sort_keys(node_keys, sorted_idx, pts_scaled);
  vector<uint32> unique_idx;
  unique_key(node_keys, unique_idx);

  // build octree structure
  build_structure(node_keys);

  // set nnum_[], nnum_cum_[], nnum_nempty_[] and ptr_dis_[]
  calc_node_num();

  // average the signal for the last octree layer
  calc_signal(point_cloud, pts_scaled, sorted_idx, unique_idx);

  // average the signal for the octher octree layer
  if (oct_info_.locations(OctreeInfo::kFeature) == -1) {
    covered_depth_nodes();

    bool calc_norm_err = oct_info_.is_adaptive();
    bool calc_dist_err = oct_info_.is_adaptive() && oct_info_.has_displace();
    calc_signal(calc_norm_err, calc_dist_err);
  }

  // generate split label
  if (oct_info_.has_property(OctreeInfo::kSplit)) {
    calc_split_label();
  }

  // serialization
  serialize();

  trim_octree();
}

void Octree::clear(int depth) {
  keys_.clear();
  children_.clear();
  displacement_.clear();
  split_labels_.clear();
  avg_normals_.clear();
  avg_features_.clear();
  avg_fpfh_.clear();
  avg_roughness_.clear();
  avg_pts_.clear();
  avg_labels_.clear();
  max_label_ = 0;
  buffer_.clear();
  info_ = nullptr;
  dnum_.clear();
  didx_.clear();
  normal_err_.clear();
  distance_err_.clear();

  if (depth == 0) return;
  keys_.resize(depth + 1);
  children_.resize(depth + 1);
  displacement_.resize(depth + 1);
  split_labels_.resize(depth + 1);
  avg_normals_.resize(depth + 1);
  avg_features_.resize(depth + 1);
  avg_fpfh_.resize(depth+1);
  avg_roughness_.resize(depth+1);
  avg_pts_.resize(depth + 1);
  avg_labels_.resize(depth + 1);
  dnum_.resize(depth + 1);
  didx_.resize(depth + 1);
  normal_err_.resize(depth + 1);
  distance_err_.resize(depth + 1);
}

void Octree::normalize_pts(vector<float>& pts_scaled, const Points& point_cloud) {
  const float* bbmin = oct_info_.bbmin();
  const float* pts = point_cloud.ptr(PtsInfo::kPoint);
  const int npt = point_cloud.info().pt_num();
  const float mul = float(1 << oct_info_.depth()) / oct_info_.bbox_max_width();
  pts_scaled.resize(3 * npt);

  // normalize the points into the range [0, 1 << depth_) using bbox_width
  #pragma omp parallel for
  for (int i = 0; i < npt; i++) {
    int i3 = i * 3;
    for (int j = 0; j < 3; j++) {
      pts_scaled[i3 + j] = (pts[i3 + j] - bbmin[j]) * mul;
    }
  }
}

void Octree::sort_keys(vector<uint32>& sorted_keys, vector<uint32>& sorted_idx,
    const vector<float>& pts_scaled) {

  // compute the code
  int depth_ = oct_info_.depth();
  int npt = pts_scaled.size() / 3;
  vector<uint64> code(npt);
  #pragma omp parallel for
  for (int i = 0; i < npt; i++) {
    // compute key
    uint32 pt[3], key;
    for (int j = 0; j < 3; ++j) {
      pt[j] = static_cast<uint32>(pts_scaled[3 * i + j]);
    }
    compute_key(key, pt, depth_);

    // generate code
    uint32* ptr = reinterpret_cast<uint32*>(&code[i]);
    ptr[0] = i;
    ptr[1] = key;
  }

  // sort all the code
  std::sort(code.begin(), code.end());

  // unpack the code
  sorted_keys.resize(npt);
  sorted_idx.resize(npt);
  #pragma omp parallel for
  for (int i = 0; i < npt; i++) {
    uint32* ptr = reinterpret_cast<uint32*>(&code[i]);
    sorted_idx[i] = ptr[0];
    sorted_keys[i] = ptr[1];
  }
}

void Octree::build_structure(vector<uint32>& node_keys) {
  const int depth_ = oct_info_.depth();
  const int full_layer_ = oct_info_.full_layer();
  children_.resize(depth_ + 1);
  keys_.resize(depth_ + 1);

  // layer 0 to full_layer_: the octree is full in these layers
  for (int curr_depth = 0; curr_depth <= full_layer_; curr_depth++) {
    vector<int>& children = children_[curr_depth];
    vector<uint32>& keys = keys_[curr_depth];

    int n = 1 << 3 * curr_depth;
    keys.resize(n, -1); children.resize(n, -1);
    for (int i = 0; i < n; i++) {
      keys[i] = i;
      if (curr_depth != full_layer_) {
        children[i] = i;
      }
    }
  }

  // layer depth_ to full_layer_
  for (int curr_depth = depth_; curr_depth > full_layer_; --curr_depth) {
    // compute parent key, i.e. keys of layer (curr_depth -1)
    int n = node_keys.size();
    vector<uint32> parent_keys(n);
    #pragma omp parallel for
    for (int i = 0; i < n; i++) {
      parent_keys[i] = node_keys[i] >> 3;
    }

    // compute unique parent key
    vector<uint32> parent_pidx;
    unique_key(parent_keys, parent_pidx);

    // augment children keys and create nodes
    int np = parent_keys.size();
    int nch = np << 3;
    vector<int>& children = children_[curr_depth];
    vector<uint32>& keys = keys_[curr_depth];
    children.resize(nch, -1);
    keys.resize(nch, 0);

    for (int i = 0; i < nch; i++) {
      int j = i >> 3;
      keys[i] = (parent_keys[j] << 3) | (i % 8);
    }

    // compute base address for each node
    vector<uint32> addr(nch);
    for (int i = 0; i < np; i++) {
      for (uint32 j = parent_pidx[i]; j < parent_pidx[i + 1]; j++) {
        addr[j] = i << 3;
      }
    }

    // set children pointer and parent pointer
    #pragma omp parallel for
    for (int i = 0; i < n; i++) {
      // address
      uint32 k = (node_keys[i] & 7u) | addr[i];

      // set children pointer for layer curr_depth
      children[k] = i;
    }

    // save data and prepare for the following iteration
    node_keys.swap(parent_keys);
  }

  // set the children for the layer full_layer_
  // Now the node_keys are the key for full_layer
  if (depth_ > full_layer_) {
    for (int i = 0; i < node_keys.size(); i++) {
      uint32 j = node_keys[i];
      children_[full_layer_][j] = i;
    }
  }
}

void Octree::calc_node_num() {
  const int depth = oct_info_.depth();

  vector<int> node_num(depth + 1, 0);
  for (int d = 0; d <= depth; ++d) {
    node_num[d] = keys_[d].size();
  }

  vector<int> node_num_nempty(depth + 1, 0);
  for (int d = 0; d <= depth; ++d) {
    // find the last element which is not equal to -1
    const vector<int>& children_d = children_[d];
    for (int i = node_num[d] - 1; i >= 0; i--) {
      if (children_d[i] != -1) {
        node_num_nempty[d] = children_d[i] + 1;
        break;
      }
    }
  }

  oct_info_.set_nnum(node_num.data());
  oct_info_.set_nempty(node_num_nempty.data());
  oct_info_.set_nnum_cum();
  oct_info_.set_ptr_dis(); // !!! note: call this function to update the ptr
}

// compute the average signal for the last octree layer
void Octree::calc_signal(const Points& point_cloud, const vector<float>& pts_scaled,
    const vector<uint32>& sorted_idx, const vector<uint32>& unique_idx) {
  int depth = oct_info_.depth();
  const float* normals = point_cloud.ptr(PtsInfo::kNormal);
  const float* features = point_cloud.ptr(PtsInfo::kFeature);
  const float* fpfh = point_cloud.ptr(PtsInfo::KFPFH);
  const float* roughness = point_cloud.ptr(PtsInfo::KRoughness);
  const float* labels = point_cloud.ptr(PtsInfo::kLabel);
  const int nnum = oct_info_.nnum(depth);

  const vector<int>& children = children_[depth];
  if (normals != nullptr) {
    const int channel = point_cloud.info().channel(PtsInfo::kNormal);
    avg_normals_[depth].assign(channel * nnum, 0.0f);

    #pragma omp parallel for
    for (int i = 0; i < nnum; i++) {
      int t = children[i];
      if (node_type(t) == kLeaf) continue;

      vector<float> avg_normal(channel, 0.0f);
      for (uint32 j = unique_idx[t]; j < unique_idx[t + 1]; j++) {
        int h = sorted_idx[j];
        for (int c = 0; c < channel; ++c) {
          avg_normal[c] += normals[channel * h + c];
        }
      }

      float factor = ESP;
      for (int c = 0; c < channel; ++c) {
        factor += avg_normal[c] * avg_normal[c];
      }
      factor = sqrtf(factor);
      for (int c = 0; c < channel; ++c) {
        avg_normals_[depth][c * nnum + i] = avg_normal[c] / factor;
      }
    }
  }

  if (features != nullptr) {
    const int channel = point_cloud.info().channel(PtsInfo::kFeature);
    avg_features_[depth].assign(channel * nnum, 0.0f);

    #pragma omp parallel for
    for (int i = 0; i < nnum; i++) {
      int t = children[i];
      if (node_type(t) == kLeaf) continue;

      vector<float> avg_feature(channel, 0.0f);
      for (uint32 j = unique_idx[t]; j < unique_idx[t + 1]; j++) {
        int h = sorted_idx[j];
        for (int c = 0; c < channel; ++c) {
          avg_feature[c] += features[channel * h + c];
        }
      }

      float factor = unique_idx[t + 1] - unique_idx[t] + ESP;
      for (int c = 0; c < channel; ++c) {
        avg_features_[depth][c * nnum + i] = avg_feature[c] / factor;
      }
    }
  }

  if (fpfh != nullptr) {
	  const int channel = point_cloud.info().channel(PtsInfo::KFPFH);
	  avg_fpfh_[depth].assign(channel * nnum, 0.0f);

#pragma omp parallel for
	  for (int i = 0; i < nnum; i++) {
		  int t = children[i];
		  if (node_type(t) == kLeaf) continue;

		  vector<float> avg_fpfh(channel, 0.0f);
		  for (uint32 j = unique_idx[t]; j < unique_idx[t + 1]; j++) {
			  int h = sorted_idx[j];
			  for (int c = 0; c < channel; ++c) {
				  avg_fpfh[c] += fpfh[channel * h + c];
			  }
		  }

		  float factor = unique_idx[t + 1] - unique_idx[t] + ESP;
		  for (int c = 0; c < channel; ++c) {
			  avg_fpfh_[depth][c * nnum + i] = avg_fpfh[c] / factor;
		  }
	  }
  }

  if (roughness != nullptr) {
	  const int channel = point_cloud.info().channel(PtsInfo::KRoughness);
	  avg_roughness_[depth].assign(channel * nnum, 0.0f);

#pragma omp parallel for
	  for (int i = 0; i < nnum; i++) {
		  int t = children[i];
		  if (node_type(t) == kLeaf) continue;

		  vector<float> avg_roughness(channel, 0.0f);
		  for (uint32 j = unique_idx[t]; j < unique_idx[t + 1]; j++) {
			  int h = sorted_idx[j];
			  for (int c = 0; c < channel; ++c) {
				  avg_roughness[c] += roughness[channel * h + c];
			  }
		  }

		  float factor = unique_idx[t + 1] - unique_idx[t] + ESP;
		  for (int c = 0; c < channel; ++c) {
			  avg_roughness_[depth][c * nnum + i] = avg_roughness[c] / factor;
		  }
	  }
  }

  if (labels != nullptr) {
    // the channel of label is fixed as 1
    avg_labels_[depth].assign(nnum, -1.0f);   // initialize as -1
    const int npt = point_cloud.info().pt_num();
    max_label_ = static_cast<int>(*std::max_element(labels, labels + npt)) + 1;

    #pragma omp parallel for
    for (int i = 0; i < nnum; i++) {
      int t = children[i];
      if (node_type(t) == kLeaf) continue;

      vector<int> avg_label(max_label_, 0);
      for (uint32 j = unique_idx[t]; j < unique_idx[t + 1]; j++) {
        int h = sorted_idx[j];
        avg_label[static_cast<int>(labels[h])] += 1;
      }

      avg_labels_[depth][i] = static_cast<float>(std::distance(avg_label.begin(),
                  std::max_element(avg_label.begin(), avg_label.end())));
    }
  }

  if (oct_info_.has_displace() && normals != nullptr) {
    const int channel = 3;
    const float mul = 1.1547f; // = 2.0f / sqrt(3.0f)
    avg_pts_[depth].assign(nnum * channel, 0.0f);
    displacement_[depth].assign(nnum, 0.0f);

    #pragma omp parallel for
    for (int i = 0; i < nnum; i++) {
      int t = children[i];
      if (node_type(t) == kLeaf) continue;

      float avg_pt[3] = { 0.0f, 0.0f, 0.0f };
      for (uint32 j = unique_idx[t]; j < unique_idx[t + 1]; j++) {
        int h = sorted_idx[j];
        for (int c = 0; c < 3; ++c) {
          avg_pt[c] += pts_scaled[3 * h + c];
        }
      }

      float dis = 0.0f;
      float factor = unique_idx[t + 1] - unique_idx[t] + ESP;
      for (int c = 0; c < 3; ++c) {
        avg_pt[c] /= factor;

        float fract_part = 0.0f, int_part = 0.0f;
        fract_part = std::modf(avg_pt[c], &int_part);
        dis += (fract_part - 0.5f) * avg_normals_[depth][c * nnum + i];

        avg_pts_[depth][c * nnum + i] = avg_pt[c];
      }
      displacement_[depth][i] = dis * mul; // !!! note the *mul* !!!
    }
  }
}

void Octree::calc_signal(const bool calc_normal_err, const bool calc_dist_err) {
  const int depth = oct_info_.depth();
  const int depth_adp = oct_info_.adaptive_layer();
  const int nnum_depth = oct_info_.nnum(depth);
  const float imul = 2.0f / sqrtf(3.0f);
  const vector<int>& children_depth = children_[depth];
  const vector<float>& normal_depth = avg_normals_[depth];
  const vector<float>& pt_depth = avg_pts_[depth];
  const vector<float>& feature_depth = avg_features_[depth];
  const vector<float>& fpfh_depth = avg_fpfh_[depth];
  const vector<float>& roughness_depth = avg_roughness_[depth];
  const vector<float>& label_depth = avg_labels_[depth];

  const int channel_pt = pt_depth.size() / nnum_depth;
  const int channel_normal = normal_depth.size() / nnum_depth;
  const int channel_feature = feature_depth.size() / nnum_depth;
  const int channel_fpfh = fpfh_depth.size() / nnum_depth;
  const int channel_roughness = roughness_depth.size() / nnum_depth;
  const int channel_label = label_depth.size() / nnum_depth;

  const bool has_pt = !pt_depth.empty();
  const bool has_dis = !displacement_[depth].empty();
  const bool has_normal = !normal_depth.empty();
  const bool has_feature = !feature_depth.empty();
  const bool has_fpfh = !fpfh_depth.empty();
  const bool has_roughness = !roughness_depth.empty();
  const bool has_label = !label_depth.empty();

  if (calc_normal_err) normal_err_[depth].resize(nnum_depth, 1.0e20f);
  if (calc_dist_err) distance_err_[depth].resize(nnum_depth, 1.0e20f);

  for (int d = depth - 1; d >= 0; --d) {
    const vector<int>& dnum_d = dnum_[d];
    const vector<int>& didx_d = didx_[d];
    const vector<int>& children_d = children_[d];
    const vector<uint32>& key_d = keys_[d];
    const float scale = static_cast<float>(1 << (depth - d));

    vector<float>& normal_d = avg_normals_[d];
    vector<float>& pt_d = avg_pts_[d];
    vector<float>& label_d = avg_labels_[d];
    vector<float>& feature_d = avg_features_[d];
	vector<float>& fpfh_d = avg_fpfh_[d];
	vector<float>& roughness_d = avg_roughness_[d];
    vector<float>& displacement_d = displacement_[d];
    vector<float>& normal_err_d = normal_err_[d];
    vector<float>& distance_err_d = distance_err_[d];

    const int nnum_d = oct_info_.nnum(d);
    if (has_normal) normal_d.assign(nnum_d * channel_normal, 0.0f);
    if (has_pt) pt_d.assign(nnum_d * channel_pt, 0.0f);
    if (has_feature) feature_d.assign(nnum_d * channel_feature, 0.0f);
	if (has_fpfh) fpfh_d.assign(nnum_d * channel_fpfh, 0.0f);
	if (has_roughness) roughness_d.assign(nnum_d * channel_roughness, 0.0f);
    if (has_label) label_d.assign(nnum_d * channel_label, -1.0f);// !!! init as -1
    if (has_dis) displacement_d.assign(nnum_d, 0.0f);
    if (calc_normal_err) normal_err_d.assign(nnum_d, 1.0e20f);   // !!! initialized
    if (calc_dist_err) distance_err_d.assign(nnum_d, 1.0e20f);   // !!! as 1.0e20f

    for (int i = 0; i < nnum_d; ++i) {
      if (node_type(children_d[i]) == kLeaf) continue;

      vector<float> n_avg(channel_normal, 0.0f);
      if (has_normal) {
        for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
          if (node_type(children_depth[j]) == kLeaf) continue;
          for (int c = 0; c < channel_normal; ++c) {
            n_avg[c] += normal_depth[c * nnum_depth + j];
          }
        }

        float len = ESP;
        for (int c = 0; c < channel_normal; ++c) {
          len += n_avg[c] * n_avg[c];
        }
        len = sqrtf(len);
        for (int c = 0; c < channel_normal; ++c) {
          n_avg[c] /= len;
          normal_d[c * nnum_d + i] = n_avg[c];  // output
        }
      }

      float count = ESP; // the non-empty leaf node in the finest layer
      for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
        if (node_type(children_depth[j]) != kLeaf) count += 1.0f;
      }

      vector<float> pt_avg(channel_pt, 0.0f);
      if (has_pt) {
        for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
          if (node_type(children_depth[j]) == kLeaf) continue;
          for (int c = 0; c < channel_pt; ++c) {
            pt_avg[c] += pt_depth[c * nnum_depth + j];
          }
        }

        for (int c = 0; c < channel_pt; ++c) {
          pt_avg[c] /= count * scale;         // !!! note the scale
          pt_d[c * nnum_d + i] = pt_avg[c];   // output
        }
      }

      vector<float> f_avg(channel_feature, 0.0f);
      if (has_feature) {
        for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
          if (node_type(children_depth[j]) == kLeaf) continue;
          for (int c = 0; c < channel_feature; ++c) {
            f_avg[c] += feature_depth[c * nnum_depth + j];
          }
        }

        for (int c = 0; c < channel_feature; ++c) {
          f_avg[c] /= count;
          feature_d[c * nnum_d + i] = f_avg[c]; // output
        }
      }

	  vector<float> fpfh_avg(channel_fpfh, 0.0f);
	  if (has_fpfh) {
		  for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
			  if (node_type(children_depth[j]) == kLeaf) continue;
			  for (int c = 0; c < channel_fpfh; ++c) {
				  fpfh_avg[c] += fpfh_depth[c * nnum_depth + j];
			  }
		  }

		  for (int c = 0; c < channel_fpfh; ++c) {
			  fpfh_avg[c] /= count;
			  fpfh_d[c * nnum_d + i] = fpfh_avg[c]; // output
		  }
	  }

	  vector<float> r_avg(channel_roughness, 0.0f);
	  if (has_roughness) {
		  for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
			  if (node_type(children_depth[j]) == kLeaf) continue;
			  for (int c = 0; c < channel_roughness; ++c) {
				  r_avg[c] += roughness_depth[c * nnum_depth + j];
			  }
		  }

		  for (int c = 0; c < channel_roughness; ++c) {
			  r_avg[c] /= count;
			  roughness_d[c * nnum_d + i] = r_avg[c]; // output
		  }
	  }

      vector<int> l_avg(max_label_, 0);
      if (has_label) {
        for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
          if (node_type(children_depth[j]) == kLeaf) continue;
          l_avg[static_cast<int>(label_depth[j])] += 1;
        }
        label_d[i] = static_cast<float>(std::distance(l_avg.begin(),
                    std::max_element(l_avg.begin(), l_avg.end())));
      }

      uint32 ptu_base[3];
      compute_pt(ptu_base, key_d[i], d);
      float pt_base[3] = { ptu_base[0], ptu_base[1], ptu_base[2] };
      if (has_dis) {
        float dis_avg = 0.0f;
        for (int c = 0; c < 3; ++c) {
          float fract_part = pt_avg[c] - pt_base[c];
          dis_avg += (fract_part - 0.5f) * n_avg[c];
        }
        displacement_d[i] = dis_avg * imul; // IMPORTANT: RESCALE
      }

      float nm_err = 0.0f;
      if (calc_normal_err && has_normal && d >= depth_adp) {
        for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
          if (node_type(children_depth[j]) == kLeaf)  continue;
          for (int c = 0; c < 3; ++c) {
            float tmp = normal_depth[c * nnum_depth + j] - n_avg[c];
            nm_err += tmp * tmp;
          }
        }
        nm_err /= count;
        normal_err_d[i] = nm_err;
      }

      if (calc_dist_err && has_pt && d >= depth_adp) {
        // the error from the original geometry to the averaged geometry
        float distance_max1 = -1.0f;
        // !!! note the scale
        float pt_avg1[3] = { pt_avg[0] * scale, pt_avg[1] * scale, pt_avg[2] * scale };
        for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
          if (node_type(children_depth[j]) == kLeaf) continue;

          float dis = 0.0f;
          for (int c = 0; c < 3; ++c) {
            dis += (pt_depth[c * nnum_depth + j] - pt_avg1[c]) * n_avg[c];
          }
          dis = abs(dis);
          if (dis > distance_max1) distance_max1 = dis;
        }

        // the error from the averaged geometry to the original geometry
        float distance_max2 = -1;
        vector<float> vtx;
        intersect_cube(vtx, pt_avg.data(), pt_base, n_avg.data());
        if (vtx.empty()) distance_max2 = 5.0e10f; // !!! the degenerated case, ||n_avg|| == 0
        for (auto& v : vtx) v *= scale;           // !!! note the scale
        for (int k = 0; k < vtx.size() / 3; ++k) {
          // min
          float distance_min = 1.0e30f;
          for (int j = didx_d[i]; j < didx_d[i] + dnum_d[i]; ++j) {
            if (node_type(children_depth[j]) == kLeaf)  continue;
            float dis = 0.0f;
            for (int c = 0; c < 3; ++c) {
              float ptc = pt_depth[c * nnum_depth + j] - vtx[3 * k + c];
              dis += ptc * ptc;
            }
            dis = sqrtf(dis);
            if (dis < distance_min) distance_min = dis;
          }

          // max
          if (distance_min > distance_max2) distance_max2 = distance_min;
        }

        distance_err_d[i] = std::max<float>(distance_max2, distance_max1);
      }
    }
  }
}

bool Octree::save(const std::string& filename)
{
    std::ofstream outfile(filename, std::ios::binary);
    if (!outfile) return false;

    save(outfile);
    outfile.close();

    return true;
}

void Octree::save(std::ostream& stream) {
  int depth_ = oct_info_.depth();
  int full_layer_ = oct_info_.full_layer();

  vector<int> node_num;
  for (auto& keys : keys_) {
    node_num.push_back(keys.size());
  }

  vector<int> node_num_accu(depth_ + 2, 0);
  for (int i = 1; i < depth_ + 2; ++i) {
    node_num_accu[i] = node_num_accu[i - 1] + node_num[i - 1];
  }
  int total_node_num = node_num_accu[depth_ + 1];
  int final_node_num = node_num[depth_];

  // calc key
  std::vector<int> key(total_node_num), children(total_node_num);
  int idx = 0;
  for (int d = 0; d <= depth_; ++d) {
    vector<uint32>& keys = keys_[d];
    for (int i = 0; i < keys.size(); ++i) {
      // calc point
      uint32 k = keys[i], pt[3];
      compute_pt(pt, k, d);

      // compress
      unsigned char* ptr = reinterpret_cast<unsigned char*>(&key[idx]);
      ptr[0] = static_cast<unsigned char>(pt[0]);
      ptr[1] = static_cast<unsigned char>(pt[1]);
      ptr[2] = static_cast<unsigned char>(pt[2]);
      ptr[3] = static_cast<unsigned char>(d);

      // children
      children[idx] = children_[d][i];

      // update index
      idx++;
    }
  }

  // write
  stream.write((char*)&total_node_num, sizeof(int));
  stream.write((char*)&final_node_num, sizeof(int));
  stream.write((char*)&depth_, sizeof(int));
  stream.write((char*)&full_layer_, sizeof(int));
  stream.write((char*)node_num.data(), sizeof(int) * (depth_ + 1));
  stream.write((char*)node_num_accu.data(), sizeof(int) * (depth_ + 2));
  stream.write((char*)key.data(), sizeof(int) * total_node_num);
  stream.write((char*)children.data(), sizeof(int) * total_node_num);
  stream.write((char*)avg_normals_[depth_].data(), sizeof(float)*avg_normals_[depth_].size());
  /*stream.write((char*)avg_features_[depth_].data(), sizeof(float)*avg_features_[depth_].size());
  stream.write((char*)avg_fpfh_[depth_].data(), sizeof(float)*avg_fpfh_[depth_].size());
  stream.write((char*)avg_roughness_[depth_].data(), sizeof(float)*avg_roughness_[depth_].size());*/
  stream.write((char*)displacement_[depth_].data(), sizeof(float)*displacement_[depth_].size());
  stream.write((char*)avg_labels_[depth_].data(), sizeof(float)*avg_labels_[depth_].size());
}

//void Octree::set_bbox(const float* bbmin, const float* bbmax) {
//  float center[3];
//  bbox_width_ = -1.0e20f;
//  for (int i = 0; i < 3; ++i) {
//    float dis = bbmax[i] - bbmin[i];
//    if (dis > bbox_width_) bbox_width_ = dis;
//    center[i] = (bbmin[i] + bbmax[i]) * 0.5f;
//  }
//
//  // deal with degenarated case
//  if (bbox_width_ == 0.0) bbox_width_ = ESP;
//
//  // set the bounding box and place the object in the center
//  float radius = bbox_width_ * 0.5f;
//  for (int i = 0; i < 3; ++i) {
//    bbmax_[i] = center[i] + radius;
//    bbmin_[i] = center[i] - radius;
//  }
//}

void Octree::unique_key(vector<uint32>& keys, vector<uint32>& idx) {
  idx.clear();
  idx.push_back(0);

  int n = keys.size(), j = 1;
  for (int i = 1; i < n; i++) {
    if (keys[i] != keys[i - 1]) {
      idx.push_back(i);
      keys[j++] = keys[i];
    }
  }

  keys.resize(j);
  idx.push_back(n);
}


void Octree::serialize() {
  const int sz = oct_info_.sizeof_octree();
  buffer_.resize(sz, 0);
  info_ = reinterpret_cast<OctreeInfo*>(buffer_.data());
  *info_ = oct_info_;

  // concatenate the avg_normals_, avg_features_,avg_fpfh_,avg_roughness_ and displacement_ into features
  const int depth = oct_info_.depth();
  vector<vector<float> > features = avg_normals_;
  for (int d = 0; d <= depth; ++d) {
    features[d].insert(features[d].end(), displacement_[d].begin(), displacement_[d].end());
    features[d].insert(features[d].end(), avg_features_[d].begin(), avg_features_[d].end());
	features[d].insert(features[d].end(), avg_fpfh_[d].begin(), avg_fpfh_[d].end());
	features[d].insert(features[d].end(), avg_roughness_[d].begin(), avg_roughness_[d].end());
  }

#define SERIALIZE_PROPERTY(Dtype, Ptype, Var)                                 \
  if (oct_info_.has_property(Ptype)) {                                        \
    Dtype* ptr = reinterpret_cast<Dtype*>(mutable_ptr(Ptype, 0));             \
    serialize<Dtype>(ptr, Var, oct_info_.locations(Ptype));                   \
  }                                                                           \

  if (oct_info_.key2xyz()) {
    vector<vector<uint32> > xyz;
    key_to_xyz(xyz);
    SERIALIZE_PROPERTY(uint32, OctreeInfo::kKey, xyz);
  } else {
    SERIALIZE_PROPERTY(uint32, OctreeInfo::kKey, keys_);
  }
  SERIALIZE_PROPERTY(int, OctreeInfo::kChild, children_);
  SERIALIZE_PROPERTY(float, OctreeInfo::kFeature, features);
  SERIALIZE_PROPERTY(float, OctreeInfo::kLabel, avg_labels_);
  SERIALIZE_PROPERTY(float, OctreeInfo::kSplit, split_labels_);
}

template<typename Dtype>
void Octree::serialize(Dtype* des, const vector<vector<Dtype> >& src, const int location) {
  if (location == -1) {
    for (int d = 0; d <= oct_info_.depth(); ++d) {
      des = std::copy(src[d].begin(), src[d].end(), des);
    }
  } else {
    std::copy(src[location].begin(), src[location].end(), des);
  }
}

void Octree::covered_depth_nodes() {
  // init
  const int depth_ = oct_info_.depth();
  for (int d = 0; d <= depth_; ++d) {
    int nnum = oct_info_.nnum(d);
    dnum_[d].assign(nnum, 0);
    didx_[d].assign(nnum, -1);
  }

  //layer-depth_
  int nnum = oct_info_.nnum(depth_);
  for (int i = 0; i < nnum; ++i) {
    dnum_[depth_][i] = 1;
    didx_[depth_][i] = i;
  }

  // layer-(depth_-1)
  nnum = oct_info_.nnum(depth_ - 1);
  for (int i = 0; i < nnum; ++i) {
    int t = children_[depth_ - 1][i];
    if (node_type(t) == kLeaf) continue;
    dnum_[depth_ - 1][i] = 8;
    didx_[depth_ - 1][i] = t * 8;
  }

  // layer-(depth-2) to layer-0
  for (int d = depth_ - 2; d >= 0; --d) {
    nnum = oct_info_.nnum(d);
    const vector<int> children_d = children_[d];
    for (int i = 0; i < nnum; ++i) {
      int t = children_d[i];
      if (node_type(t) == kLeaf) continue;
      t *= 8;
      for (int j = 0; j < 8; ++j) {
        dnum_[d][i] += dnum_[d + 1][t + j];
      }
      for (int j = 0; j < 8; ++j) {
        if (didx_[d + 1][t + j] != -1) {
          didx_[d][i] = didx_[d + 1][t + j];
          break;
        }
      }
    }
  }
}

void Octree::trim_octree() {
  if (!oct_info_.is_adaptive()) return;
  const int depth = oct_info_.depth();
  const int depth_adp = oct_info_.adaptive_layer();
  const float th_dist = oct_info_.threshold_distance();
  const float th_norm = oct_info_.threshold_normal();
  const bool has_dis = oct_info_.has_displace();

  // generate the drop flag
  enum TrimType { kDrop = 0, kDropChildren = 1,  kKeep = 2 };
  vector<vector<TrimType> > drop(depth + 1);
  for (int d = 0; d <= depth; ++d) {
    drop[d].resize(oct_info_.nnum(d), kKeep);
  }
  for (int d = depth_adp; d <= depth; ++d) {
    int nnum_dp = oct_info_.nnum(d - 1);
    const vector<int>& children_d = children_[d];
    const vector<int>& children_dp = children_[d - 1];
    vector<TrimType>& drop_d = drop[d];
    vector<TrimType>& drop_dp = drop[d - 1];

    bool all_drop = true;
    // generate the drop flag
    for (int i = 0; i < nnum_dp; ++i) {
      int t = children_dp[i];
      if (node_type(t) == kLeaf) continue;

      // generate the drop flag for 8 children nodes:
      // drop the node if its parent node is kDrop or kDropChildren,
      // set the node as kDropChildren if the error is smaller than a threshold
      for (int j = 0; j < 8; ++j) {
        int idx = t * 8 + j;
        if (drop_dp[i] == kKeep) {
          // note that for all the leaf nodes and the finest nodes,
          // distance_err_[d][i] is equal to 1.0e20f, so if it enters the following
          // "if" body, the node_type(children_d[idx]) must be kInternelNode
          //if (distance_err_[d][idx] < th_dist) {
          if ((!has_dis || (has_dis && distance_err_[d][idx] < th_dist)) &&
              normal_err_[d][idx] < th_norm) {
            drop_d[idx] = kDropChildren;
          }
        } else {
          drop_d[idx] = kDrop;
        }

        if (all_drop) {
          // all_drop is false: there is at least one internal node which is kept
          all_drop = !(drop_d[idx] == kKeep &&
                  node_type(children_d[idx]) == kInternelNode);
        }
      }
    }

    // make sure that there is at least one octree node in each layer
    if (all_drop) {
      int max_idx = 0;
      float max_err = -1.0f;
      for (int i = 0; i < nnum_dp; ++i) {
        int t = children_dp[i];
        if (node_type(t) == kLeaf || drop_dp[i] != kKeep) continue;

        for (int j = 0; j < 8; ++j) {
          int idx = t * 8 + j;
          if (node_type(children_d[idx]) == kInternelNode &&
              normal_err_[d][idx] > max_err) {
            max_err = normal_err_[d][idx];
            max_idx = idx;
          }
        }
      }
      drop_d[max_idx] = kKeep;
    }
  }

  // trim the octree
  for (int d = depth_adp; d <= depth; ++d) {
    int nnum_d = oct_info_.nnum(d);
    const vector<TrimType>& drop_d = drop[d];

    vector<uint32> key;
    for (int i = 0; i < nnum_d; ++i) {
      if (drop_d[i] == kDrop) continue;
      key.push_back(keys_[d][i]);
    }
    keys_[d].swap(key);

    vector<int> children;
    for (int i = 0, id = 0; i < nnum_d; ++i) {
      if (drop_d[i] == kDrop) continue;
      int ch = (drop_d[i] == kKeep && node_type(children_[d][i]) != kLeaf) ? id++ : -1;
      children.push_back(ch);
    }
    children_[d].swap(children);

    auto trim_data = [&](vector<float>& signal) {
      vector<float> data;
      int channel = signal.size() / nnum_d;
      if (channel == 0) return;
      for (int i = 0; i < nnum_d; ++i) {
        if (drop_d[i] == kDrop) continue;
        for (int c = 0; c < channel; ++c) {
          data.push_back(signal[c * nnum_d + i]);
        }
      }
      //signal.swap(data);
      // transpose
      int num = data.size() / channel;
      signal.resize(data.size());
      for (int i = 0; i < num; ++i) {
        for (int c = 0; c < channel; ++c) {
          signal[c * num + i] = data[i * channel + c];
        }
      }
    };

    trim_data(displacement_[d]);
    trim_data(avg_normals_[d]);
    trim_data(avg_features_[d]);
	trim_data(avg_fpfh_[d]);
	trim_data(avg_roughness_[d]);
    trim_data(avg_labels_[d]);
  }

  // update the node number
  calc_node_num();

  // generate split label
  if (oct_info_.has_property(OctreeInfo::kSplit)) {
    calc_split_label();
  }

  // serialization
  serialize();
}

void Octree::key_to_xyz(vector<vector<uint32> >& xyz) {
  const int depth = oct_info_.depth();
  const int channel = oct_info_.channel(OctreeInfo::kKey);
  xyz.resize(depth + 1);
  for (int d = 0; d <= depth; ++d) {
    int nnum = oct_info_.nnum(d);
    xyz[d].resize(nnum * channel, 0);
    uint32* xyz_d = xyz[d].data();
    for (int i = 0; i < nnum; ++i) {
      uint32 pt[3] = { 0, 0, 0 };
      compute_pt(pt, keys_[d][i], d);

      if (channel == 1) {
        unsigned char* ptr = reinterpret_cast<unsigned char*>(xyz_d + i);
        for (int c = 0; c < 3; ++c) {
          ptr[c] = static_cast<unsigned char>(pt[c]);
        }
      } else {
        unsigned short* ptr = reinterpret_cast<unsigned short*>(xyz_d + 2 * i);
        for (int c = 0; c < 3; ++c) {
          ptr[c] = static_cast<unsigned short>(pt[c]);
        }
      }
    }
  }
}

void Octree::calc_split_label() {
  const int depth = oct_info_.depth();
  const int channel = oct_info_.channel(OctreeInfo::kSplit); // is 1 (by default)
  const bool adaptive = oct_info_.is_adaptive();

  for (int d = 0; d <= depth; ++d) {
    int nnum_d = oct_info_.nnum(d);
    split_labels_[d].assign(nnum_d, 1);       // initialize as 1 (non-empty, split)
    for (int i = 0; i < nnum_d; ++i) {
      if (node_type(children_[d][i]) == kLeaf) {
        split_labels_[d][i] = 0;              // empty node
        if (adaptive) {
          float t = abs(avg_normals_[d][i]) + abs(avg_normals_[d][nnum_d + i]) +
              abs(avg_normals_[d][2 * nnum_d + i]);
          if (t != 0) split_labels_[d][i] = 2; // surface-well-approximated
        }
      }
    }
  }
}
