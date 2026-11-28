fn main() {
    println!("cargo:rustc-link-arg-cdylib=/DELAYLOAD:leonida-probe-delayed.dll");
    println!("cargo:rustc-link-lib=delayimp");
}
