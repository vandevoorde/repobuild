// Copyright 2013
// Author: Christopher Van Arsdale

#ifndef _REPOBUILD_NODES_AUTOCONF_H__
#define _REPOBUILD_NODES_AUTOCONF_H__

#include <string>
#include "repobuild/nodes/node.h"

namespace repobuild {

class AutoconfNode : public Node {
 public:
  AutoconfNode(const TargetInfo& t,
                   const Input& i)
      : Node(t, i) {
  }
  virtual ~AutoconfNode() {}
  virtual void Parse(BuildFile* file, const BuildFileNode& input);
  virtual void WriteMakefile(const std::vector<const Node*>& all_deps,
                             Makefile* out) const {
    WriteBaseUserTarget(out);
  }
};

}  // namespace repobuild

# endif  // _REPOBUILD_NODES_AUTOCONF_H__
