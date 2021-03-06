import unittest
from types import *
from masarclient.control import Control as Control


'''

Unittests for masarService/python/masarclient/control.py

'''


class TestControl(unittest.TestCase):

    def setUp(self):
        self.control = Control()

    '''
    Tests both default value assignment and getter operation for LimitLow
    '''
    def testGetLimitLow(self):
        self.assertEqual(0.0, self.control.getLimitLow())

    '''
    Tests both default value assignment and getter operation for LimitHigh
    '''
    def testGetLimitHigh(self):
        self.assertEqual(0.0, self.control.getLimitHigh())

    '''
    Tests both default value assignment and getter operation for MinStep
    '''
    def testGetMinStep(self):
        self.assertEqual(0.0, self.control.getMinStep())

    '''
    Tests setter for LimitLow, requires getLimitLow
    '''
    def testSetLimitLow(self):
        limitLowTestValue = -10.0  # Default test value can be changed here
        self.control.setLimitLow(limitLowTestValue)
        self.assertEqual(self.control.getLimitLow(), limitLowTestValue)

    '''
    Tests setter for LimitHigh, requires getLimitHigh
    '''
    def testSetLimitHigh(self):
        limitHighTestValue = 10.0  # Default test value can be changed here
        self.control.setLimitHigh(limitHighTestValue)
        self.assertEqual(self.control.getLimitHigh(), limitHighTestValue)

    '''
    Tests setter for MinStep, requires getMinStep
    '''
    def testSetMinStep(self):
        minStepTestValue = 1.0  # Default test value can be changed here
        self.control.setMinStep(minStepTestValue)
        self.assertEqual(self.control.getMinStep(), minStepTestValue)

    '''
    Tests Control's non-default constructor. Full use test.
    '''
    def testNonDefaultConstructor(self):
        limitLowTestValue = -10.0  # Default test values may need to be changed
        limitHighTestValue = 10.0
        minStepTestValue = 1.0
        control = Control(limitLowTestValue, limitHighTestValue, minStepTestValue)
        self.assertEqual(control.getLimitLow(), limitLowTestValue)
        self.assertEqual(control.getLimitHigh(), limitHighTestValue)
        self.assertEqual(control.getMinStep(), minStepTestValue)

    if __name__ == '__main__':
        unittest.main()
