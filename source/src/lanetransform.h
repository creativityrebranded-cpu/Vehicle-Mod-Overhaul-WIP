#pragma once
#include "lanevolume.h"
#include <array>
#include <iomanip>
#include <set>
#include <sstream>

// Mod-owned authoring transforms. Route points/section identities stay intact.
// Separate record from lane_plan so all previous geometry versions remain readable.
namespace lanetransform {
using Vec=lanevolume::Vec3;
using Matrix=std::array<double,9>;
inline Matrix Identity(){return {1,0,0,0,1,0,0,0,1};}
inline Vec Apply(const Matrix& m,Vec p){return {m[0]*p.x+m[1]*p.y+m[2]*p.z,m[3]*p.x+m[4]*p.y+m[5]*p.z,m[6]*p.x+m[7]*p.y+m[8]*p.z};}
inline Matrix Multiply(const Matrix& a,const Matrix& b){Matrix out{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)out[i*3+j]+=a[i*3+k]*b[k*3+j];return out;}
inline double Determinant(const Matrix& m){return m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6]);}
inline Matrix Inverse(const Matrix& m){const double d=Determinant(m);if(std::fabs(d)<1e-12)return Identity();return {(m[4]*m[8]-m[5]*m[7])/d,(m[2]*m[7]-m[1]*m[8])/d,(m[1]*m[5]-m[2]*m[4])/d,(m[5]*m[6]-m[3]*m[8])/d,(m[0]*m[8]-m[2]*m[6])/d,(m[2]*m[3]-m[0]*m[5])/d,(m[3]*m[7]-m[4]*m[6])/d,(m[1]*m[6]-m[0]*m[7])/d,(m[0]*m[4]-m[1]*m[3])/d};}
inline Matrix Rotation(int axis,double radians){const double c=std::cos(radians),s=std::sin(radians);if(axis==0)return {1,0,0,0,c,-s,0,s,c};if(axis==1)return {c,0,s,0,1,0,-s,0,c};return {c,-s,0,s,c,0,0,0,1};}
struct Record {int section=-1,lane=0;Vec pivot{},shift{};Matrix matrix=Identity();double blend=15;};
struct Plan {std::vector<Record> records;std::string unsupported;};
inline bool Valid(const Record& r){if(r.section< -1||r.section>100000||r.lane<0||r.lane>=16||!routemap::Finite(r.pivot)||!routemap::Finite(r.shift)||!std::isfinite(r.blend)||r.blend<0||r.blend>10000)return false;for(double v:r.matrix)if(!std::isfinite(v)||std::fabs(v)>10000)return false;const double determinant=std::fabs(Determinant(r.matrix));return determinant>1e-9&&determinant<1e12;}
inline bool Valid(const Plan& p){if(!p.unsupported.empty()||p.records.size()>256)return false;std::set<std::pair<int,int>> scopes;for(const auto& r:p.records)if(!Valid(r)||!scopes.insert({r.section,r.lane}).second)return false;return true;}
inline Vec Point(const Record& r,Vec p){return lanevolume::Add(lanevolume::Add(r.pivot,Apply(r.matrix,lanevolume::Sub(p,r.pivot))),r.shift);}
inline Record& Edit(Plan& p,int lane,int section,Vec pivot){for(auto& r:p.records)if(r.lane==lane&&r.section==section)return r;p.records.push_back({section,lane,pivot});return p.records.back();}
inline void Translate(Record& r,Vec delta){r.shift=lanevolume::Add(r.shift,delta);}
inline void Compose(Record& r,const Matrix& m,Vec centre){r.shift=lanevolume::Sub(lanevolume::Add(centre,Apply(m,lanevolume::Sub(lanevolume::Add(r.pivot,r.shift),centre))),r.pivot);r.matrix=Multiply(m,r.matrix);}
inline void Parent(Plan& p,const Matrix& m,Vec centre,Vec delta={},double blendScale=1){const auto inverse=Inverse(m);for(auto& r:p.records){r.pivot=lanevolume::Add(lanevolume::Add(centre,Apply(m,lanevolume::Sub(r.pivot,centre))),delta);r.shift=Apply(m,r.shift);r.matrix=Multiply(Multiply(m,r.matrix),inverse);r.blend=std::clamp(r.blend*blendScale,0.0,10000.0);}}
inline std::string Write(const Plan& p){if(!p.unsupported.empty())return p.unsupported;if(p.records.empty())return {};std::ostringstream out;out<<std::setprecision(17)<<1<<' '<<p.records.size();for(const auto& r:p.records){out<<" | "<<r.section<<' '<<r.lane<<' '<<r.pivot.x<<' '<<r.pivot.y<<' '<<r.pivot.z<<' '<<r.shift.x<<' '<<r.shift.y<<' '<<r.shift.z<<' '<<r.blend;for(double v:r.matrix)out<<' '<<v;}return out.str();}
inline Plan Read(const std::string& text){Plan p;if(text.empty())return p;Plan bad;bad.unsupported=text;auto copy=text;std::replace(copy.begin(),copy.end(),'|',' ');std::istringstream in(copy);int version;std::size_t count;if(!(in>>version>>count)||version!=1||count>256)return bad;for(std::size_t i=0;i<count;++i){Record r;if(!(in>>r.section>>r.lane>>r.pivot.x>>r.pivot.y>>r.pivot.z>>r.shift.x>>r.shift.y>>r.shift.z>>r.blend))return bad;for(auto& v:r.matrix)if(!(in>>v))return bad;p.records.push_back(r);}std::string extra;if(in>>extra||!Valid(p))return bad;return p;}
inline void Insert(Plan& p,int at){std::vector<Record> split;for(auto& r:p.records){if(at>0&&r.section==at-1){auto copy=r;copy.section=at;split.push_back(copy);}if(r.section>=at)++r.section;}p.records.insert(p.records.end(),split.begin(),split.end());}
inline void Remove(Plan& p,int at){std::erase_if(p.records,[&](const auto& r){return r.section==at;});for(auto& r:p.records)if(r.section>at)--r.section;}
inline Plan Remap(const Plan& p,const std::vector<int>& sections){auto out=p;std::erase_if(out.records,[](const auto& r){return r.section>=0;});for(std::size_t i=0;i<sections.size();++i)for(auto r:p.records)if(r.section>=0&&r.section==sections[i]){r.section=int(i);out.records.push_back(r);}return out;}
inline double Weight(double at,double begin,double end,double total,double blend,bool closed){if(closed&&total>0){while(at<begin)at+=total;while(at>=begin+total)at-=total;}if(at>=begin&&at<=end)return 1;double distance=at<begin?begin-at:at-end;if(closed&&total>0)distance=std::min(distance,begin+total-at);if(blend<=0||distance>=blend)return 0;const double t=1-distance/blend;return t*t*(3-2*t);}
}
