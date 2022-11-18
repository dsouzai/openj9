public class UnitTester {

	enum State {
		INITIALIZED,
		WAITING,
		RUNMETHOD,
		EXIT
	}

	volatile static State state = State.INITIALIZED;
	final int SLEEP_SECONDS = 5;

	public UnitTester() {
	}

	public void emptyMethod() {
	}

	public void runTests() throws Throwable {
		state = State.WAITING;
		while (state != State.EXIT) {
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
