public class UnitTester {

	final static int NATIVE_INITIALIZED = 0;
	final static int JAVA_INITIALIZED = 1;
	final static int RUNMETHOD = 2;
	final static int EXIT = 3;

	final int SLEEP_SECONDS = 5;

	public UnitTester() {
	}

	public void emptyMethod() {
	}

	public void runTests() throws Throwable {
		System.out.println("Starting");

		informReady();

		int state = getState();
		while (state != EXIT) {
			switch (state) {
				case RUNMETHOD:
					emptyMethod();
					break;
				default:
					Thread.sleep(SLEEP_SECONDS);
			}
		}

		System.out.println("Exiting");
	}
	
	public static void main(String args[]) throws Throwable {
		UnitTester unitTester = new UnitTester();
		try {
			unitTester.runTests();
		} catch (Exception e) {
			System.out.println("Failed to run tests");
		}
	}

	private native void informReady();

	private native int getState();
}
