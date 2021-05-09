#ifndef _OCTREE_POINTS_
#define _OCTREE_POINTS_

#include <vector>
#include <string>

using std::vector;
using std::string;

class PtsInfo {
 public:
  enum PropType { kPoint = 1, kNormal = 2, kFeature = 4, KFPFH = 8, KRoughness = 16, kLabel = 32 };
  static const int kPTypeNum = 6;
  static const char kMagicStr[16];

 public:
  PtsInfo() { reset(); }
  void reset();
  bool check_format(string& msg) const;
  bool has_property(PropType ptype) const {
    return (content_flags_ & ptype) != 0;
  }

  int pt_num() const { return pt_num_; }
  int channel(PropType ptype) const;
  int ptr_dis(PropType ptype) const;
  int sizeof_points() const { return ptr_dis_[kPTypeNum]; }

  void set_pt_num(int num) { pt_num_ = num; }
  void set_channel(PropType ptype, const int ch);
  void set_ptr_dis();

 protected:
  int property_index(PropType ptype) const;

 protected:
  char magic_str_[16];
  int pt_num_;
  int content_flags_;
  int channels_[8];
  int ptr_dis_[8];
};

struct PointsData
{
    int npt;
    const float* pts;
    const float* normals;
    const float* seg;
    const float* features;
	const float* fpfh;
	const float* roughness;
    const float* labels;
};

struct PointsBounds
{
    float radius;
    float center[3];
};

class Points {
 public:
  Points() : info_(nullptr), buffer_() {}
  bool is_empty() const { return info_ == nullptr || info_->pt_num() == 0; }

  // the pts must not be empty, the labels may be empty,
  // the normals & features must not be empty at the same time.
  bool set_points(const vector<float>& pts, const vector<float>& normals,
      const vector<float>& features = vector<float>(),
	  const vector<float>& fpfh = vector<float>(),
	  const vector<float>& roughness = vector<float>(),
      const vector<float>& labels = vector<float>());
  void set_points(vector<char>& data); // swap data and buffer_

  PointsData get_points_data() const;
  PointsBounds get_points_bounds() const;

  bool read_points(const string& filename);
  bool write_points(const string& filename) const;
  bool write_ply(const string& filename) const;

  const PtsInfo& info() const { return *info_; }
  const float* ptr(PtsInfo::PropType ptype) const;
  float* mutable_ptr(PtsInfo::PropType ptype);

  void center_about(const float* center);
  void displace(const float dis);
  void rotate(const float angle, const float* axis);
  void transform(const float* transformation_matrix);

 protected:
  PtsInfo* info_;
  vector<char> buffer_;
};

#endif // _OCTREE_POINTS_
