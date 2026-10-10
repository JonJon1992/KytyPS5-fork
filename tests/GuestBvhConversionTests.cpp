#include "graphics/host_gpu/renderer/rt/guestBvh.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using namespace Libs::Graphics::RT;
void Check(bool ok, const char* text) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text); std::abort(); } }
void Word(BvhSnapshot& s, size_t index, uint32_t value) { std::memcpy(s.bytes.data()+index*4,&value,4); }
BvhSnapshot Fixture() {
 BvhSnapshot s; s.address=0x10000; s.descriptor={0x100,0,4,0x81000000}; s.root=5; s.bytes.resize(5*64);
 for(unsigned c=0;c<4;c++) Word(s,c,16+c); // four triangle kinds in the same leaf
 const std::array<float,15> v{-1,-1,2, 1,-1,2, 0,1,2, 2,1,2, 0,3,2};
 for(unsigned i=0;i<v.size();i++) Word(s,32+i,std::bit_cast<uint32_t>(v[i]));
 Word(s,47,0x04040404);
 for(unsigned c=0;c<4;c++) for(unsigned a=0;a<6;a++)
  Word(s,4+c*6+a,std::bit_cast<uint32_t>(a<3?-4.f:4.f));
 return s;
}
int main() {
 auto s=Fixture(); auto a=Convert(s);
 Check(bool(a),"box32 with four triangle kinds");
 Check(a.geometry.primitives.size()==4 && a.geometry.vertices.size()==12,"primitive count");
 Check(a.geometry.primitives[3].node==19 && a.geometry.primitives[3].flags==0x04040404,"guest IDs");
 Check(a.geometry.vertices[9]==Vertex{0,1,2} && a.geometry.vertices[10]==Vertex{0,3,2} &&
       a.geometry.vertices[11]==Vertex{-1,-1,2},"kind3 mapping");
 auto half=s; half.root=4;
 for(unsigned c=0;c<4;c++) for(unsigned i=0;i<3;i++) Word(half,4+c*3+i, i==0?0xc400c400:i==1?0x4400c400:0x44004400);
 Check(bool(Convert(half)),"box16 conversion");
 auto half_nan=half; Word(half_nan,4,0x7e00c400);
 Check(Convert(half_nan).reject==Reject::NonFinite,"NaN half bound");
 auto bad_type=s; bad_type.descriptor[3]=0x71000000;
 Check(Convert(bad_type).reject==Reject::Descriptor,"wrong descriptor type");
 auto reserved=s; reserved.descriptor[1]|=0x100;
 Check(Convert(reserved).reject==Reject::Descriptor,"unknown descriptor bits fall back");
 auto high_base=s; high_base.descriptor[1]=0xff;
 Check(Convert(high_base).reject==Reject::SnapshotRange,"descriptor base outside aperture");
 auto shared=s; Word(shared,1,16);
 Check(Convert(shared).reject==Reject::CycleOrSharedNode,"shared triangle fallback");
 auto sparse=s; Word(sparse,0,~0u);
 Check(Convert(sparse).geometry.primitives.size()==3,"sentinel child skipped");
 auto leaf=s; leaf.root=18;
 Check(Convert(leaf).geometry.primitives.size()==1,"subtree can start at a triangle");
 auto cycle=s; Word(cycle,0,5); Check(Convert(cycle).reject==Reject::CycleOrSharedNode,"cycle");
 auto instance=s; Word(instance,0,22); Check(Convert(instance).reject==Reject::UnsupportedNode,"instance fallback");
 auto bounds=s; bounds.descriptor[2]=0; Check(Convert(bounds).reject==Reject::NodeBounds,"full node needs second block");
 auto unmapped=s; unmapped.bytes.resize(64); Check(Convert(unmapped).reject==Reject::Unmapped,"truncated snapshot");
 auto nan=s; Word(nan,32,0x7fc00000); Check(Convert(nan).reject==Reject::NonFinite,"NaN vertex");
 auto partial=s; Word(partial,44,0x7f800000);
 auto failed=Convert(partial);
 Check(failed.reject==Reject::NonFinite && failed.geometry.vertices.empty() &&
       failed.geometry.primitives.empty(),"late rejection hides all partial geometry");
 auto outside=s; outside.address=(1ull<<40)-64; Check(Convert(outside).reject==Reject::SnapshotRange,"aperture overflow");
 Check(Convert(s,2).reject==Reject::Budget,"bounded traversal");
 Check(bool(Convert(s,5)) && Convert(s,4).reject==Reject::Budget,"exact node budget");
 auto empty=s; for(unsigned c=0;c<4;c++)Word(empty,c,~0u); Check(Convert(empty).reject==Reject::Empty,"empty scene");
 PreparationCache cache;
 auto first=cache.Prepare(s);Check(first.change==Change::Rebuild,"first build");
 auto same=cache.Prepare(s);Check(same.change==Change::Reuse&&same.geometry==first.geometry,"unchanged snapshot reuses immutable geometry");
 auto moved=s;Word(moved,34,std::bit_cast<uint32_t>(3.f));
 auto update=cache.Prepare(moved);Check(update.change==Change::Update,"positions only allow update");
 Check(first.geometry->vertices[0].z==2,"old frame snapshot remains immutable");
 auto flags=moved;Word(flags,47,123);
 Check(cache.Prepare(flags).change==Change::Metadata,"flags only do not rebuild AS");
 auto topology=flags;Word(topology,0,19);Word(topology,3,16);
 Check(cache.Prepare(topology).change==Change::Rebuild,"primitive order changes require rebuild");
 Check(cache.Prepare(instance).change==Change::Reject,"reject unsupported subtree");
 Check(cache.Prepare(s).change==Change::Rebuild,"reject retires preparation cache");
 auto grow=s; grow.descriptor[1]|=1u<<23;
 Check(cache.Prepare(grow).change==Change::Rebuild,"changed descriptor invalidates preparation");
 auto relocated=grow; relocated.address+=256; relocated.descriptor[0]++;
 Check(cache.Prepare(relocated).change==Change::Rebuild,"relocation invalidates preparation");
 cache.Clear();
 Check(cache.Prepare(s).change==Change::Rebuild,"explicit clear");
 auto signed_zero=s; Word(signed_zero,38,0x80000000);
 Check(cache.Prepare(signed_zero).change==Change::Update,"signed zero positions compare by bits");
 PreparationCache tiny(8);Check(tiny.Prepare(s).reject==Reject::Budget,"snapshot byte budget");
 std::puts("GuestBvhConversionTests: conversion, fallback, immutable reuse/update/rebuild passed");
}
