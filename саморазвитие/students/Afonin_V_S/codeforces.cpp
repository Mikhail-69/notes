#include <algorithm>
#include <bitset>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

struct segtree {
  vector<long long> tree;
  segtree(vector<long long> &vec) {
    tree.assign(vec.size() * 4, 1e9);
    build(vec, 0, 0, vec.size() - 1);
  }
  long long build(vector<long long> &vec, long long v, long long tl,
                  long long tr) {
    if (tr - tl == 0) {
      return tree[v] = vec[tl];
    }
    long long mid = (tl + tr) / 2;
    return tree[v] = min(build(vec, v * 2 + 1, tl, mid),
                         build(vec, v * 2 + 2, mid + 1, tr));
  }
  long long value(long long v, long long tl, long long tr, long long l,
                  long long r) {
    if (tl >= l && tr <= r) {
      return tree[v];
    }
    if (tl > r || tr < l) {
      return 1e9;
    } else {
      long long mid = (tl + tr) / 2;
      return min(value(2 * v + 1, tl, mid, l, r),
                 value(2 * v + 2, mid + 1, tr, l, r));
    }
  }
  void change(long long v, long long tl, long long tr, long long index,
              long long value) {
    if (tl == tr) {
      tree[v] = value;
      return;
    }
    long long mid = (tl + tr) / 2;
    if (index <= mid) {
      change(v * 2 + 1, tl, mid, index, value);
    } else {
      change(v * 2 + 2, mid + 1, tr, index, value);
    }
    tree[v] = min(tree[v * 2 + 1], tree[v * 2 + 2]);
  }
};

int main() {
  freopen("rvq.in", "r", stdin);
  freopen("rvq.out", "w", stdout);
  vector<long long> a(100001);
  for (long long i = 1; i <= 100000; i++) {
    a[i] += (i * i) % 12345;
    a[i] += (i * i * i) % 23456;
  }
  vector<long long> b(100001);
  for (long long i = 1; i <= 100000; i++) {
    b[i] = a[i] * (-1);
  }
  segtree stmin(a);
  segtree stmax(b);
  int k;
  if (!(cin >> k)) return 0;
  while (k--) {
    long long x, y;
    cin >> x >> y;
    if (x > 0) {
      long long mn = stmin.value(0, 0, 100000, x, y);
      long long mx = -stmax.value(0, 0, 100000, x, y);
      cout << mx - mn << "\n";
    } else {
      long long idx = abs(x);
      stmin.change(0, 0, 100000, idx, y);
      stmax.change(0, 0, 100000, idx, -y);
    }
  }
  return 0;
}