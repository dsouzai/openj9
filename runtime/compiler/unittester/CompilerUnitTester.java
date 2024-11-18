public class CompilerUnitTester {

	static {
		System.loadLibrary("openj9-compiler-unittester");
	}

	private native void openj9_compiler_handshake();

	public static void main (String[] args) throws Throwable {
		CompilerUnitTester unittester = new CompilerUnitTester();
		unittester.openj9_compiler_handshake();

		while (true) {
			Thread.sleep(1000);
		}

	}

}
