// This file if part of the llir-opt project.
// Licensing information can be found in the LICENSE file.
// (C) 2018 Nandor Licker. All rights reserved.

#pragma once

#include <vector>
#include <memory>

#include "core/adt/id.h"



/**
 * Union-find data structure to manage nodes.
 */
template <typename T>
class UnionFind {
public:
  UnionFind() : size_(0) {}

  template <typename... Args>
  ID<T> Emplace(Args... args)
  {
    size_++;
    unsigned n = entries_.size();
    ID<T> id(n);
    entries_.emplace_back(
        n,
        std::make_unique<T>(id, std::forward<Args>(args)...)
    );
    return id;
  }

  ID<T> Union(ID<T> idA, ID<T> idB)
  {
    unsigned idxA = Find(idA);
    unsigned idxB = Find(idB);
    if (idxA == idxB) {
      return idxB;
    }

    size_--;

    Entry &entryA = entries_[idxA];
    Entry &entryB = entries_[idxB];
    T *a = entryA.Element.get();
    T *b = entryB.Element.get();

    if (entryA.Rank < entryB.Rank) {
      entryA.Parent = idxB;
      b->Union(*a);
      entries_[idxA].Element = nullptr;
      return idxB;
    } else {
      entryB.Parent = idxA;
      a->Union(*b);
      entries_[idxB].Element = nullptr;
      if (entryA.Rank == entryB.Rank) {
        entryA.Rank += 1;
      }
      return idxA;
    }
  }

  T *Map(ID<T> id) const
  {
    return entries_[Find(id)].Element.get();
  }

  ID<T> Find(ID<T> id) const
  {
    unsigned root = id;
    while (entries_[root].Parent != root) {
      root = entries_[root].Parent;
    }
    while (entries_[id].Parent != id) {
      unsigned parent = entries_[id].Parent;
      entries_[id].Parent = root;
      id = parent;
    }
    return id;
  }


  unsigned Size() const { return size_; }

private:
  /// Union-Find entry.
  struct Entry {
    /// Parent ID.
    mutable unsigned Parent;
    /// Union-Find Rank.
    mutable unsigned Rank;
    /// Element or nullptr if unified.
    std::unique_ptr<T> Element;

    Entry(unsigned n, std::unique_ptr<T> &&element)
      : Parent(n)
      , Rank(0)
      , Element(std::move(element))
    {
    }
  };

  /// Mapping from indices to entries.
  std::vector<Entry> entries_;
  /// Number of distinct nodes.
  unsigned size_;
};
