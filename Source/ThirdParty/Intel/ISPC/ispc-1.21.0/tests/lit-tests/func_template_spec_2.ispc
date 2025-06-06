// Check that function specialization can be declared, then used, then defined.
// RUN: %{ispc}  %s --emit-llvm-text --target=host --nostdlib -o - | FileCheck %s

// Specialized function
// CHECK-LABEL: define {{.*}} <{{[0-9]*}} x i32> @goo___vyivyf___vyivyf(<{{[0-9]*}} x i32> %argGooOne, <{{[0-9]*}} x float> %argGooTwo)
// Check that implementation of specialized function is generated from specialization
// CHECK: fmul

// CHECK-LABEL: define <{{[0-9]*}} x float> @foo
// CHECK: call {{.*}} <{{[0-9]*}} x i32> @goo___vyivyf___vyivyf(<{{[0-9]*}} x i32> %argFoo0, <{{[0-9]*}} x float> %argFoo1)
// CHECK: call {{.*}} <{{[0-9]*}} x i32> @goo___vyivyi___vyivyi(<{{[0-9]*}} x i32> %argFoo0, <{{[0-9]*}} x i32> %argFoo0)

// Instantiated function
// CHECK-LABEL: define {{.*}} <{{[0-9]*}} x i32> @goo___vyivyi___vyivyi(<{{[0-9]*}} x i32> %argGooOne, <{{[0-9]*}} x i32> %argGooTwo)
// Check that implementation of instantiated function is generated from primary template
// CHECK: add

template <typename T, typename C> noinline int goo(T argGooOne, C argGooTwo) {
    return argGooOne + argGooTwo;
}

template <> noinline int goo<int, float>(int argGooOne, float argGooTwo);

float foo(int argFoo0, float argFoo1) {
    float a = goo<int, float>(argFoo0, argFoo1);
    int b = goo<int, int>(argFoo0, argFoo0);
    return a + b;
}

template <> noinline int goo<int, float>(int argGooOne, float argGooTwo) {
  return argGooOne * argGooTwo;
}

