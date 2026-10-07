// Windows: the FNC Reborn icon on the exe.
fn main() {
    println!("cargo:rerun-if-changed=../project/res/FNCReborn.ico");
    #[cfg(windows)]
    {
        let mut res = winresource::WindowsResource::new();
        res.set_icon("../project/res/FNCReborn.ico");
        res.set("ProductName", "FNC Reborn Setup");
        res.set("FileDescription", "FNC Reborn Setup");
        if let Err(e) = res.compile() {
            println!("cargo:warning=no icon: {e}");
        }
    }
}
