 # Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import common
from common import TestDriver
from common import IntegrationTest
from decorators import ChromeVersionBetweenInclusiveM
from decorators import ChromeVersionEqualOrAfterM
from emulation_server import BlackHoleHandler
from emulation_server import InvalidTLSHandler
from emulation_server import TCPResetHandler
from emulation_server import TLSResetHandler

class ProxyConnection(IntegrationTest):

  @ChromeVersionEqualOrAfterM(63)
  def testTLSInjectionAfterHandshake(self):
    port = common.GetOpenPort()
    with TestDriver() as t:
      t.AddChromeArg('--enable-spdy-proxy-auth')
      # The server should be 127.0.0.1, not localhost because the two are
      # treated differently in Chrome internals. Using localhost invalidates the
      # test.
      t.AddChromeArg(
        '--data-reduction-proxy-http-proxies=https://127.0.0.1:%d' % port)
      t.AddChromeArg(
        '--force-fieldtrials=DataReductionProxyConfigService/Disabled')
      t.UseEmulationServer(InvalidTLSHandler, port=port)

      t.LoadURL('http://check.googlezip.net/test.html')
      responses = t.GetHTTPResponses()
      # Expect responses with a bypass on a bad proxy. If the test failed, the
      # next assertion will fail because there will be no responses.
      self.assertEqual(2, len(responses))
      for response in responses:
        self.assertNotHasChromeProxyViaHeader(response)
      self.assertTrue(t.SleepUntilHistogramHasEntry('DataReductionProxy.'
        'InvalidResponseHeadersReceived.NetError'))

  @ChromeVersionEqualOrAfterM(75)
  def testTCPReset(self):
    port = common.GetOpenPort()
    with TestDriver() as t:
      t.AddChromeArg('--enable-spdy-proxy-auth')
      # The server should be 127.0.0.1, not localhost because the two are
      # treated differently in Chrome internals. Using localhost invalidates the
      # test.
      t.UseNetLog()
      t.AddChromeArg(
        '--data-reduction-proxy-http-proxies=http://127.0.0.1:%d' % port)
      t.AddChromeArg(
        '--force-fieldtrials=DataReductionProxyConfigService/Disabled')
      t.UseEmulationServer(TCPResetHandler, port=port)

      t.LoadURL('http://check.googlezip.net/test.html')
      responses = t.GetHTTPResponses()
      # Expect responses with a bypass on a bad proxy. If the test failed, the
      # next assertion will fail because there will be no responses.
      self.assertEqual(2, len(responses))
      for response in responses:
        self.assertNotHasChromeProxyViaHeader(response)
      self.assertTrue(
        t.SleepUntilHistogramHasEntry('DataReductionProxy.WarmupURL.NetError',
          sleep_intervals=10))
      histogram = t.GetBrowserHistogram('DataReductionProxy.'
        'WarmupURLFetcherCallback.SuccessfulFetch.InsecureProxy.Core')
      self.assertEqual(1, histogram['count'])

  @ChromeVersionEqualOrAfterM(63)
  def testTLSReset(self):
    port = common.GetOpenPort()
    with TestDriver() as t:
      t.AddChromeArg('--enable-spdy-proxy-auth')
      t.AddChromeArg('--allow-insecure-localhost')
      # The server should be 127.0.0.1, not localhost because the two are
      # treated differently in Chrome internals. Using localhost invalidates the
      # test.
      t.AddChromeArg(
        '--data-reduction-proxy-http-proxies=https://127.0.0.1:%d' % port)
      t.AddChromeArg(
        '--force-fieldtrials=DataReductionProxyConfigService/Disabled')
      t.UseEmulationServer(TLSResetHandler, port=port)

      t.LoadURL('http://check.googlezip.net/test.html')
      responses = t.GetHTTPResponses()
      # Expect responses with a bypass on a bad proxy. If the test failed, the
      # next assertion will fail because there will be no responses.
      self.assertEqual(2, len(responses))
      for response in responses:
        self.assertNotHasChromeProxyViaHeader(response)

  @ChromeVersionEqualOrAfterM(75)
  def testTCPBlackhole(self):
    port = common.GetOpenPort()
    with TestDriver() as t:
      t.UseNetLog()
      t.AddChromeArg('--enable-spdy-proxy-auth')
      t.EnableChromeFeature(
        'DataReductionProxyRobustConnection<DataReductionProxyRobustConnection')
      t.AddChromeArg('--force-fieldtrials='
        'DataReductionProxyRobustConnection/Enabled')
      t.AddChromeArg('--force-fieldtrial-params='
        'DataReductionProxyRobustConnection.Enabled:'
        'warmup_fetch_callback_enabled/true')
      t.AddChromeArg('--force-effective-connection-type=4G')
      # The server should be 127.0.0.1, not localhost because the two are
      # treated differently in Chrome internals. Using localhost invalidates the
      # test.
      t.AddChromeArg(
        '--data-reduction-proxy-http-proxies=http://127.0.0.1:%d' % port)

      t.UseEmulationServer(BlackHoleHandler, port=port)
      # Start Chrome and wait for the warmup fetcher timeout (30 seconds).
      t.LoadURL('data:,')
      self.assertTrue(
        t.SleepUntilHistogramHasEntry('DataReductionProxy.WarmupURL.NetError',
          sleep_intervals=40))

      # Check the WarmupURL Callback was called.
      histogram = t.GetBrowserHistogram('DataReductionProxy.'
        'WarmupURLFetcherCallback.SuccessfulFetch.InsecureProxy.Core')
      self.assertEqual(1, histogram['count'])

      # Verify DRP was not used.
      t.LoadURL('http://check.googlezip.net/test.html')
      responses = t.GetHTTPResponses()
      self.assertEqual(2, len(responses))
      for response in responses:
        self.assertNotHasChromeProxyViaHeader(response)

if __name__ == '__main__':
  IntegrationTest.RunAllTests()
