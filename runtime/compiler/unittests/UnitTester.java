public class UnitTester {

	final static int INITIALIZED = 0;
	final static int WAITING = 1;
	final static int RUNMETHOD = 2;
	final static int EXIT = 3;

	final int SLEEP_SECONDS = 5;

	volatile static int state = INITIALIZED;

	public UnitTester() {
	}

	public void emptyMethod() {
	}

	public void runTests() throws Throwable {
		state = WAITING;
		while (state != EXIT) {
			switch (state) {
				case RUNMETHOD:
					emptyMethod();
					break;
				default:
					Thread.sleep(SLEEP_SECONDS);
			}
		}
	}
	
	public static void main(String args[]) throws Throwable {
		UnitTester unitTester = new UnitTester();
		try {
			unitTester.runTests();
		} catch (Exception e) {
			System.out.println("Failed to run tests");
		}
	}
}
