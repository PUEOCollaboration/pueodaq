
R_LOAD_LIBRARY("build/libpueodaq.so");

void fakedaq_plot(int index, const char * format = "/tmp/fakedaq_%05d.dat")
{

  FILE * f = fopen(Form(format,index));
  pueo_daq_event_data_t d;
  int nb = fread(&d, sizeof(pueo_daq_event_data_t), 1, f);

  printf("Read %d bytes\n");

  printf(PUEO_DAQ_EVENT_HEADER_FORMAT, PUEO_DAQ_EVENT_HEADER_VALUES(d.header));

  for (int isurf = 0; isurf < 28; isurf++)
  {
    TCanvas * c = new TCanvas(Form("c%d",isurf),Form("LINK %d, SURF %d", isurf/7, isurf % 7), 1800, 1000);
    c->Divide(4,2);
    for (int ichan = 0; ichan < 8; ichan++)
    {
      c->cd(ichan+1);
      TGraph * g = new TGraph(PUEODAQ_NSAMP);
      int idx = isurf*8+chan;
      for (int i = 0; i < g->GetN(); i++)
      {
        g->SetPoint(i,i./3, d.waveform_data[idx][i]);
      }
      g->SetTitle(Form("RMS=%f", g->GetRMS(2)));
      g->Draw("alp");

    }
    c->Show();
  }

}




}
