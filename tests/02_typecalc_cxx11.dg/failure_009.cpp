/*
<testinfo>
test_generator=config/mercurium-fe-only
test_CXXFLAGS="-std=c++11"
test_compile_fail=yes
</testinfo>
*/
struct A
{
};

template <typename T>
struct B : A
{
  void foo() override { }
};

B<int> myB;
