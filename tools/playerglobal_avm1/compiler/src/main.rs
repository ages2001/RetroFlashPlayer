use rascal::{CompileOptions, FileSystemSourceProvider, ProgramBuilder, SwfOptions};
use std::path::Path;
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let provider = FileSystemSourceProvider::with_root(Path::new(&args[1]).to_path_buf());
    let compile_options = CompileOptions::default().with_swf_version(8);
    let mut builder = ProgramBuilder::new(provider).with_compile_options(compile_options);
    builder.add_script("globals.as");
    let swf_bytes = builder.build()?.compile().to_swf(&SwfOptions::default())?;
    std::fs::write(&args[2], swf_bytes)?;
    Ok(())
}
